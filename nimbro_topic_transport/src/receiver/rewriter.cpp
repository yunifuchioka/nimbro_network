// Rewrites all TF frame IDs
// Author: Max Schwarz <max.schwarz@ais.uni-bonn.de>

#include "rewriter.h"

#include <boost/filesystem.hpp>

#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/trim.hpp>

#include <ros/console.h>
#include <ros/package.h>

#include <dlfcn.h>
#include <sys/wait.h>

#include "../thread_pool.h"
#include "../subprocess.h"
#include "../topic_info.h"

#include "../../data/gadget/interface.h"

#include "tt_compile_flags.h"

namespace fs = boost::filesystem;

namespace nimbro_topic_transport
{

class Rewriter::TopicRewriter::Private
{
public:
	std::string prefix;
	void* handle = nullptr;
	MorphPtr morph = nullptr;
};

Rewriter::TopicRewriter::TopicRewriter() = default;

Rewriter::TopicRewriter::TopicRewriter(const std::string& library, const std::string& prefix)
 : m_d{std::make_unique<Private>()}
{
	ROS_INFO("Loading TopicRewriter: %s", library.c_str());

	m_d->handle = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
	if(!m_d->handle)
	{
		ROS_ERROR("Could not open topic rewriter library '%s': %s",
			library.c_str(), dlerror()
		);
		ROS_ERROR("Maybe you should try to delete the above file.");
		return;
	}

	typedef MorphPtr (*RegisterFunc)();
	auto func = reinterpret_cast<RegisterFunc>(dlsym(m_d->handle, "registerMorph"));
	if(!func)
	{
		ROS_ERROR("Could not find register function in rewriter library %s: %s",
			library.c_str(), dlerror()
		);
		ROS_ERROR("Maybe you should try to delete the above file.");
		return;
	}

	m_d->morph = func();
	m_d->prefix = prefix;
}

Rewriter::TopicRewriter::~TopicRewriter()
{
	if(m_d && m_d->handle)
		dlclose(m_d->handle);
}

std::vector<uint8_t> Rewriter::TopicRewriter::rewrite(const std::vector<uint8_t>& data) const
{
	if(!m_d)
		return {}; // Identity case (e.g. no prefix)

	if(!m_d->morph)
		return {}; // Something wrong during loading, just pass through

	return m_d->morph(data, m_d->prefix);
}

bool Rewriter::TopicRewriter::isNull() const
{
	return !m_d->morph;
}

class Rewriter::Private
{
public:
	TopicRewriter compile(const std::string& fullType, const std::string& md5)
	{
		auto split = fullType.find('/');
		if(split == std::string::npos)
		{
			ROS_ERROR("Could not split message type '%s' into parts", fullType.c_str());
			return {};
		}

		std::string system_md5 = topic_info::getMd5Sum(fullType);
		if(system_md5 != md5)
		{
			ROS_ERROR("We got a message of type '%s' with type md5 '%s', but our version is '%s'. Please make sure your messages are up to date on both systems!",
				fullType.c_str(),
				md5.c_str(),
				system_md5.c_str()
			);
			ROS_ERROR("I will disable rewriting for this message type!");
			return {};
		}

		std::string package = fullType.substr(0, split);
		std::string type = fullType.substr(split+1);

		fs::path msgBinPath = binaryPath / (package + "___" + type + "___" + md5);
		fs::path outputFile = msgBinPath / "libtopic_rewriter.so";
		fs::path inputFile = sharePath / "topic_rewriter.cpp";

		auto lastUpdate = std::max(
			fs::last_write_time(inputFile),
			fs::last_write_time(pchFile)
		);

		bool upToDate = fs::exists(outputFile) && fs::last_write_time(outputFile) > lastUpdate;

		if(upToDate)
		{
			TopicRewriter rewriter{outputFile.string(), tfPrefix};
			if(!rewriter.isNull())
				return rewriter;
		}

		// The output file does not exist, is not up to date, or cannot be loaded.
		// => Compile a new one!

		ROS_INFO("Compiling topic rewriter for topic type '%s'", fullType.c_str());

		if(!fs::exists(msgBinPath))
		{
			if(!fs::create_directories(msgBinPath))
			{
				ROS_ERROR("Could not create binary path '%s': %s", msgBinPath.c_str(), strerror(errno));
				return {};
			}
		}

		std::string tmpfile = (msgBinPath / "tmp-XXXXXX").string();
		int fd = mkstemp(tmpfile.data());

		if(fd < 0)
		{
			ROS_ERROR("Could not create temporary file '%s': %s", tmpfile.c_str(), strerror(errno));
			return {};
		}
		close(fd);

		// Run compiler
		std::vector<std::string> args{
			"g++",
			"-o", tmpfile,
			"-shared", "-fPIC",
			"-include", pchPath.string(),
			"-DMSG_INCLUDE=<" + fullType + ".h>",
			"-DMSG_PACKAGE=" + package,
			"-DMSG_TYPE=" + type,
			"-DMSG_MD5_HIGH=0x" + md5.substr(0, 16) + "ULL",
			"-DMSG_MD5_LOW=0x" + md5.substr(16) + "ULL",
			"-Winvalid-pch"
			TT_COMPILE_FLAGS
		};
		for(auto& a : includeFlags)
			args.push_back(a);
		args.push_back(inputFile.string());

		if(!subprocess::call("g++", args))
		{
			ROS_ERROR("Compilation of rewrite gadget for msg type %s failed (see stderr above). Command line was:",
				fullType.c_str()
			);

			std::stringstream ss;
			for(auto& arg : args)
			{
				if(arg.find(' ') != std::string::npos)
					ss << "'" << arg << "'";
				else
					ss << arg << " ";
			}
			ROS_ERROR("%s", ss.str().c_str());

			return {};
		}

		// This is atomic
		fs::rename(tmpfile, outputFile);

		return TopicRewriter{outputFile.string(), tfPrefix};
	}

	std::string topicPrefix;
	std::string tfPrefix;

	fs::path binaryPath;
	fs::path sharePath;
	fs::path pchPath;
	fs::path pchFile;

	std::vector<std::string> includeFlags;

	std::mutex mutex;

	using Key = std::pair<std::string, std::string>;
	std::map<Key, TopicRewriterFuture> rewriters;
};

Rewriter::Rewriter(const ros::NodeHandle& nhIn)
 : m_d{std::make_unique<Private>()}
{
	// Get configuration
	{
		ros::NodeHandle nh{nhIn};

		std::string prefix;
		if(nh.getParam("prefix", prefix))
		{
			ROS_WARN("You specified the receiver 'prefix' param. This param is deprecated, you should instead set topic_prefix and tf_prefix separately.");
			m_d->topicPrefix = prefix;
			m_d->tfPrefix = prefix;
		}

		nh.getParam("tf_prefix", m_d->tfPrefix);
		nh.getParam("topic_prefix", m_d->topicPrefix);
	}

	m_d->binaryPath = fs::path(getenv("HOME")) / ".ros" / "nimbro_topic_transport";

	auto sourcePath = subprocess::obtainOutput(
		"catkin_find",
		{"catkin_find", "--first-only", "--share", "nimbro_topic_transport"}
	);
	if(!sourcePath)
		throw std::runtime_error("Could not find the share path of nimbro_topic_transport");

	boost::algorithm::trim(*sourcePath);

	m_d->sharePath = fs::path(*sourcePath);
	if(m_d->sharePath.empty())
		throw std::runtime_error("Could not find nimbro_topic_transport package");

	// Compile the precompiled header
	{
		m_d->pchPath = m_d->sharePath / "ros_includes.h";
		m_d->pchFile = m_d->sharePath / "ros_includes.h.gch";
	}

	// Compute include dirs from CMAKE_PREFIX_PATH
	const char* CMAKE_PREFIX_PATH = getenv("CMAKE_PREFIX_PATH");
	if(!CMAKE_PREFIX_PATH)
	{
		throw std::runtime_error("CMAKE_PREFIX_PATH not defined");
	}

	std::vector<std::string> paths;
	boost::algorithm::split(paths, CMAKE_PREFIX_PATH, boost::algorithm::is_any_of(":"));

	for(const auto& path : paths)
	{
		m_d->includeFlags.push_back("-I" + path + "/include");
	}
}

Rewriter::~Rewriter()
{
	for(auto& pair : m_d->rewriters)
	{
		pair.second.wait();
	}
}

Rewriter::TopicRewriterFuture Rewriter::open(const std::string& topicType, const std::string& md5)
{
	if(m_d->tfPrefix.empty())
	{
		std::promise<TopicRewriter> promise;
		auto future = promise.get_future().share();

		promise.set_value(TopicRewriter{});
		return future;
	}

	std::unique_lock<std::mutex> lock{m_d->mutex};

	auto it = m_d->rewriters.find(Private::Key{topicType, md5});
	if(it != m_d->rewriters.end())
		return it->second;
	else
	{
		auto future = std::async(std::bind(&Private::compile, m_d.get(), topicType, md5), std::launch::async).share();

		m_d->rewriters[Private::Key{topicType, md5}] = future;

		return future;
	}
}

std::string Rewriter::rewriteTopicName(const std::string& name)
{
	return m_d->topicPrefix + name;
}

}
