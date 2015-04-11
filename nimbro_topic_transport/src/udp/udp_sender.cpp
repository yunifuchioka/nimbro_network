// UDP sender node
// Author: Max Schwarz <max.schwarz@uni-bonn.de>

#include "udp_sender.h"
#include "topic_sender.h"
#include "udp_packet.h"

#include <ros/init.h>
#include <ros/node_handle.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/timerfd.h>

#include <ros/console.h>
#include <stdio.h>
#include <errno.h>

#include <XmlRpcValue.h>

#include <signal.h>

namespace nimbro_topic_transport
{

UDPSender::UDPSender()
 : m_msgID(0)
{
	ros::NodeHandle nh("~");

	m_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if(m_fd < 0)
	{
		ROS_FATAL("Could not create socket: %s", strerror(errno));
		throw std::runtime_error(strerror(errno));
	}

	int on = 1;
	if(setsockopt(m_fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)) != 0)
	{
		ROS_FATAL("Could not enable SO_BROADCAST flag: %s", strerror(errno));
		throw std::runtime_error(strerror(errno));
	}
	
	nh.param("relay_mode", m_relayMode, false);

	std::string dest_host;
	nh.param("destination_addr", dest_host, std::string("192.168.178.255"));

	int dest_port;
	nh.param("destination_port", dest_port, 5050);

	if(nh.hasParam("source_port"))
	{
		int source_port;
		if(!nh.getParam("source_port", source_port))
		{
			ROS_FATAL("Invalid source_port");
			throw std::runtime_error("Invalid source port");
		}

		sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = INADDR_ANY;
		addr.sin_port = htons(source_port);

		if(bind(m_fd, (const sockaddr*)&addr, sizeof(addr)) != 0)
		{
			ROS_FATAL("Could not bind to source port: %s", strerror(errno));
			throw std::runtime_error(strerror(errno));
		}
	}

	memset(&m_addr, 0, sizeof(m_addr));
	m_addr.sin_addr.s_addr = inet_addr(dest_host.c_str());
	m_addr.sin_port = htons(dest_port);
	m_addr.sin_family = AF_INET;


	XmlRpc::XmlRpcValue list;
	nh.getParam("topics", list);

	ROS_ASSERT(list.getType() == XmlRpc::XmlRpcValue::TypeArray);

	for(int32_t i = 0; i < list.size(); ++i)
	{
		ROS_ASSERT(list[i].getType() == XmlRpc::XmlRpcValue::TypeStruct);
		ROS_ASSERT(list[i].hasMember("name"));

		int flags = 0;
		
		bool resend = false;

		double rate = 100.0;
		if(list[i].hasMember("rate"))
			rate = list[i]["rate"];

		if(list[i].hasMember("compress") && ((bool)list[i]["compress"]))
			flags |= UDP_FLAG_COMPRESSED;

		if(list[i].hasMember("resend") && ((bool)list[i]["resend"]))
			resend = true;

		TopicSender* sender = new TopicSender(this, &nh, list[i]["name"], rate, resend, flags);

		if(m_relayMode)
			sender->setDirectTransmissionEnabled(false);

		m_senders.push_back(sender);
	}

	nh.param("duplicate_first_packet", m_duplicateFirstPacket, false);

	if(m_relayMode)
	{
		double target_bitrate;
		if(!nh.getParam("relay_target_bitrate", target_bitrate))
		{
			throw std::runtime_error("relay mode needs relay_target_bitrate param");
		}

		double relay_control_rate;
		nh.param("relay_control_rate", relay_control_rate, 100.0);

		m_relayTokens = 0;
		m_relayIndex = 0;
		m_relayTokensPerStep = target_bitrate / 8.0 / relay_control_rate;

		m_relayThreadShouldExit = false;
		m_relayRate = relay_control_rate;
		m_relayThread = boost::thread(boost::bind(&UDPSender::relay, this));

		ROS_INFO("udp_sender: relay mode configured with control rate %f, target bitrate %f bit/s and token increment %d",
			relay_control_rate, target_bitrate, m_relayTokensPerStep
		);
	}
}

UDPSender::~UDPSender()
{
	if(m_relayMode)
	{
		m_relayThreadShouldExit = true;
		m_relayThread.join();
	}

	for(unsigned int i = 0; i < m_senders.size(); ++i)
		delete m_senders[i];
}

uint16_t UDPSender::allocateMessageID()
{
	return m_msgID++;
}

bool UDPSender::send(const void* data, uint32_t size)
{
	if(m_relayMode)
	{
		std::vector<uint8_t> packet(size);
		memcpy(packet.data(), data, size);

		m_relayBuffer.emplace_back(std::move(packet));
		return true;
	}
	else
	{
		return internalSend(data, size);
	}
}

bool UDPSender::internalSend(const void* data, uint32_t size)
{
	ros::Time now = ros::Time::now();
	ros::Duration delta = now - m_lastTime;

	if(delta < ros::Duration(0.008))
	{
		m_sleepCounter++;
		delta.sleep();

		if(m_sleepCounter > 125)
		{
			m_sleepCounter = 0;
			ROS_ERROR("UDPSender: the 8ms rate limit is limiting communication. Please send fewer data or increase the limit!");
		}
	}
	else
		m_sleepCounter = 0;

	if(sendto(m_fd, data, size, 0, (sockaddr*)&m_addr, sizeof(m_addr)) != size)
	{
		ROS_ERROR("Could not send data of size %d: %s", size, strerror(errno));
		return false;
	}

	return true;
}

void UDPSender::relay()
{
	ros::WallRate rate(m_relayRate);

	while(!m_relayThreadShouldExit)
	{
		// New tokens! Bound to 2*m_relayTokensPerStep to prevent token buildup.
		m_relayTokens = std::min<uint64_t>(
			100*m_relayTokensPerStep,
			m_relayTokens + m_relayTokensPerStep
		);

		if(m_senders.empty())
			throw std::runtime_error("No senders configured");

		// While we have enough token, send something!
		while(1)
		{
			unsigned int tries = 0;
			while(m_relayBuffer.empty())
			{
				if(tries++ == m_senders.size())
					return; // No data yet

				m_senders[m_relayIndex]->sendCurrentMessage();
				m_relayIndex = (m_relayIndex + 1) % m_senders.size();

	//			if(m_relayIndex == 0)
	//				ROS_INFO("Full circle");
			}

			const std::vector<uint8_t>& packet = m_relayBuffer.front();
			std::size_t sizeOnWire = packet.size() + 20 + 8;

			// out of tokens? Wait for next iteration.
			if(sizeOnWire > m_relayTokens)
				break;

			if(!internalSend(packet.data(), packet.size()))
			{
				ROS_ERROR("Could not send packet");
				return;
			}

			// Consume tokens
			m_relayTokens -= sizeOnWire;
			m_relayBuffer.pop_front();
		}

		rate.sleep();
	}
}

}

int main(int argc, char** argv)
{
	ros::init(argc, argv, "udp_sender");

	ros::NodeHandle nh("~");
	bool relay_mode;
	nh.param("relay_mode", relay_mode, false);

	nimbro_topic_transport::UDPSender sender;

	ros::spin();

	return 0;
}
