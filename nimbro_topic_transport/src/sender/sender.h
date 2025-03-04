// Top-level control for the sender node
// Author: Max Schwarz <max.schwarz@uni-bonn.de>

#ifndef TT_SENDER_H
#define TT_SENDER_H

#include "subscriber.h"
#include "tcp_sender.h"
#include "udp_sender.h"
#include "packetizer.h"
#include "compressor.h"
#include "../thread_pool.h"
#include <ros/ros.h>
#include <std_srvs/Trigger.h>

namespace nimbro_topic_transport
{

class Sender
{
public:
	Sender(ros::NodeHandle nh = ros::NodeHandle("~"));
	~Sender();

private:
	void advertiseTopics();
	void refreshTopicList(double rate);

	std::string stripPrefix(const std::string& topic) const;

	void initTCP(XmlRpc::XmlRpcValue& topicList);
	void initUDP(XmlRpc::XmlRpcValue& topicList);

	bool enableSendingLowPriorityTopics(std_srvs::Trigger::Request& request, std_srvs::Trigger::Response& response);
	bool disableSendingLowPriorityTopics(std_srvs::Trigger::Request& request, std_srvs::Trigger::Response& response);

	ros::NodeHandle m_nh;
	std::vector<std::unique_ptr<Subscriber>> m_subs;
	std::unique_ptr<TCPSender> m_tcp_sender;

	std::unique_ptr<UDPSender> m_udp_sender;

	ThreadPool m_threadPool;
	std::shared_ptr<Packetizer> m_packetizer;

	std::string m_stripPrefix;

	std::map<std::string, std::string> m_topicTypeMap;
	std::mutex m_topicTypeMapMutex;

	std::thread m_topicThread;
	ros::SteadyTimer m_advertiseTimer;

	ros::ServiceServer m_enableSendingLowPriorityTopicService;
	ros::ServiceServer m_disableSendingLowPriorityTopicService;
};

}

#endif
