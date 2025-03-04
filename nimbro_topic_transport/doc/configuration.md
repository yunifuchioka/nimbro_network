
Configuration
=============

`nimbro_topic_transport` is configured through ROS parameters.

Receiver parameters
-------------------

`receiver` accepts the following parameters:

Required:
 - `port` (int): UDP/TCP port to bind to (required)

Optional:
 - `drop_repeated_msgs` (bool): If a message with the same sequence number
   arrives twice, drop it. Needed in conjunction with the relay mode.
   (UDP only, default true)
 - `fec` (bool): Enable Forward Error correction (UDP only, default false)
 - `label` (string): Display a label in the visualization GUIs

Sender parameters
-----------------

`sender` accepts the following parameters:

Required:
 - `destination_addr` (string): Hostname or IP address of the destination
   machine (required)
 - `port` (int): Port number to connect to (required)
 - `source_port` (int): Source port to bind to. If not specified, the port is
   chosen by the OS.
 - `topics` (list): List of topics to be transmitted (see below)

Optional:
 - `fec` (float): If non-zero, this is the proportion of repair packets sent for
   Forward Error Correction (0.5 -> Send 50% more data). Default: 0.0.
 - `label` (string): Display a label in the visualization GUIs
 - `relay_mode` (bool): Enable relay mode, see README.md
   (UDP only, default false)
 - `relay_target_bitrate` (float): Target bitrate for relay mode (UDP only)
 - `relay_control_rate` (float): Check if new packets can be sent in relay mode
   at this rate (UDP only)

Topic configuration
-------------------

Configuration of topics to be transmitted is done on the parameter server of
the sender's side. See the example launch files for the usual setup.

Here is a list of parameters that are available per topic. The only mandatory
parameter is `name`.

 - `name`: Name of the topic to be sent over.
 - `rate`: Rate limit on messages / sec (floating point). Messages over the
   rate limit are silently dropped on the sender side. The default is 0.0
   (no rate limit).
   Note: Limiting only works well for lower rates (<20 Hz).
   (UDP only)
 - `resend`: If the sender does not get a message 1.0/`rate` after the last one,
   it will re-send the last received one. (UDP only)
 - `compress`: If true, compress the data on the wire using zstd. If you specify
   an integer, it will determine the zstd compression level.
