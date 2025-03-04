cmake_minimum_required(VERSION 3.1)

project(nimbro_topic_transport)

find_package(catkin REQUIRED COMPONENTS
	roscpp
	topic_tools
	rostest
	message_generation
	roslib
	geometry_msgs
	nav_msgs
)

add_message_files(FILES
	CompressedMsg.msg
	ReceiverStats.msg
	SenderStats.msg
	TopicBandwidth.msg
)

generate_messages(DEPENDENCIES
	std_msgs
)

catkin_package()
include_directories(${catkin_INCLUDE_DIRS})

find_package(catch_ros)

# Find zstd
find_package(PkgConfig)
pkg_check_modules(ZSTD REQUIRED libzstd>=1.1.4)

include_directories(${ZSTD_INCLUDE_DIRS})
include_directories(contrib)

set(CMAKE_CXX_STANDARD 17)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")

add_library(nimbro_topic_transport
	contrib/wirehair/wirehair.cpp
	contrib/wirehair/gf256.cpp
	contrib/wirehair/WirehairCodec.cpp
	contrib/wirehair/WirehairTools.cpp
	src/sender/compressor.cpp
	src/sender/get_route.cpp
	src/sender/packetizer.cpp
	src/sender/subscriber.cpp
	src/sender/sender.cpp
	src/sender/tcp_sender.cpp
	src/sender/udp_sender.cpp
	src/receiver/decompressor.cpp
	src/receiver/depacketizer.cpp
	src/receiver/rewriter.cpp
	src/receiver/publisher.cpp
	src/receiver/receiver.cpp
	src/receiver/tcp_receiver.cpp
	src/receiver/udp_receiver.cpp
	src/subprocess.cpp
	src/topic_info.cpp
	src/thread_pool.cpp
)
target_link_libraries(nimbro_topic_transport
	${catkin_LIBRARIES}
	${ZSTD_LIBRARIES}
)
target_compile_options(nimbro_topic_transport PUBLIC
	"-march=native"
)
target_include_directories(nimbro_topic_transport PRIVATE
	${CMAKE_CURRENT_BINARY_DIR}
)

add_library(precompiled_header OBJECT
	src/receiver/ros_includes.cpp
)
target_compile_options(precompiled_header PRIVATE
	-x c++-header
)
set_target_properties(precompiled_header PROPERTIES
	POSITION_INDEPENDENT_CODE ON

	# We need to capture all compile definitions so that later JIT compilation
	# can use the same options
	RULE_LAUNCH_COMPILE "${CMAKE_CURRENT_SOURCE_DIR}/utils/extract_compile_flags.sh ${CMAKE_CURRENT_BINARY_DIR}/tt_compile_flags.h"
)
add_dependencies(nimbro_topic_transport precompiled_header)

add_custom_command(
	OUTPUT ros_includes.h.gch
	DEPENDS precompiled_header $<TARGET_OBJECTS:precompiled_header>
	COMMAND cmake -E copy $<TARGET_OBJECTS:precompiled_header> ${CMAKE_CURRENT_BINARY_DIR}/ros_includes.h.gch
)
add_custom_target(copy_precompiled_header ALL
	DEPENDS ros_includes.h.gch
	COMMAND cmake -E copy_if_different ${CMAKE_CURRENT_BINARY_DIR}/ros_includes.h.gch ${CATKIN_DEVEL_PREFIX}/${CATKIN_PACKAGE_SHARE_DESTINATION}
)

add_custom_target(copy_gadget ALL
	COMMAND cmake -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/data/gadget/topic_rewriter.cpp ${CATKIN_DEVEL_PREFIX}/${CATKIN_PACKAGE_SHARE_DESTINATION}
)
add_custom_target(copy_gadget_interfaces ALL
	COMMAND cmake -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/data/gadget/interface.h ${CATKIN_DEVEL_PREFIX}/${CATKIN_PACKAGE_SHARE_DESTINATION}
)

add_executable(sender
	src/sender/main.cpp
)
target_link_libraries(sender
	${catkin_LIBRARIES}
	nimbro_topic_transport
)

add_executable(receiver
	src/receiver/main.cpp
)
target_link_libraries(receiver
	${catkin_LIBRARIES}
	nimbro_topic_transport
)

add_executable(transceiver
	src/transceiver.cpp
)
target_link_libraries(transceiver
	${catkin_LIBRARIES}
	nimbro_topic_transport
)

# Tools
add_executable(action_proxy
	src/action_proxy.cpp
	src/subprocess.cpp
	src/topic_info.cpp
)
target_link_libraries(action_proxy
	${catkin_LIBRARIES}
)

# Tests
if(catch_ros_FOUND)
	include_directories(${catch_ros_INCLUDE_DIRS})

	catch_add_test(unit_tests
		test/unit/test_packetizer.cpp
		test/unit/test_compression.cpp
	)
	target_link_libraries(unit_tests
		nimbro_topic_transport
	)

	catch_add_rostest_node(test_comm
		test/test_comm.cpp
	)
	target_link_libraries(test_comm
		${catkin_LIBRARIES}
	)

	add_rostest(test/topic_transport.test ARGS port:=5778 fec:=false)
	add_rostest(test/topic_transport.test ARGS port:=5779 fec:=true)
	add_rostest(test/transceiver.test ARGS port:=5780 fec:=false)
endif()

#install
install(TARGETS nimbro_topic_transport sender receiver action_proxy transceiver
        ARCHIVE DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}
        LIBRARY DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}
        RUNTIME DESTINATION ${CATKIN_PACKAGE_BIN_DESTINATION}
)

install(FILES utils/get_msg_def.py
        DESTINATION ${CATKIN_PACKAGE_BIN_DESTINATION}
)
