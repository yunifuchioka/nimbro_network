
uint8 DIR_IN = 0
uint8 DIR_OUT = 1

# Topic name
string topic

# Local node
string local_node

# Peer node (the other end)
# this is a node *name*
string destination

# See DIR_IN/DIR_OUT
uint8 direction

# Used transport (e.g. 'TCPROS')
string transport

# Current bandwidth
uint64 bits_per_second
