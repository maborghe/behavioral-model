/* -*- P4_16 -*- */
#include <core.p4>
#include <v1model.p4>


/*************************************************************************
*********************** H E A D E R S  ***********************************
*************************************************************************/

typedef bit<9>  egressSpec_t;
typedef bit<48> macAddr_t;
typedef bit<32> ip4Addr_t;

header ethernet_t {
    macAddr_t dstAddr;
    macAddr_t srcAddr;
    bit<16>   etherType;
}

header lfu_header_t {
    bit<2> update_type;
    bit<3> table_id;
    bit<48> key_field_1;
    bit<48> key_mask_1;
    bit<3> action_id;   
    bit<9> data_field_1;
}

struct headers {
    ethernet_t   ethernet;
}

struct metadata {
    bit<6> header_count;
    lfu_header_t[30] lfu_header_stack;
}

/*************************************************************************
*********************** P A R S E R  ***********************************
*************************************************************************/

parser MyParser(packet_in packet,
                out headers hdr,
                inout metadata meta,
                inout standard_metadata_t standard_metadata) {

    state start {
        transition parse_ethernet;
    }

    state parse_ethernet {
        packet.extract(hdr.ethernet);
        transition accept;
    }
}


/*************************************************************************
************   C H E C K S U M    V E R I F I C A T I O N   *************
*************************************************************************/

control MyVerifyChecksum(inout headers hdr, inout metadata meta) {   

    apply {  }
}


/*************************************************************************
**************  I N G R E S S   P R O C E S S I N G   *******************
*************************************************************************/

control MyIngress(inout headers hdr,
                  inout metadata meta,
                  inout standard_metadata_t standard_metadata) {


action add_flow(bit<3> table_id, bit<48> key_field_1, bit<3> action_id, bit<9> data_field_1) {
    meta.lfu_header_stack.push_front(1);
    meta.header_count = meta.header_count + 1;
    meta.lfu_header_stack[0].update_type = 0;
    meta.lfu_header_stack[0].table_id = table_id;
    meta.lfu_header_stack[0].key_field_1 = key_field_1;
    meta.lfu_header_stack[0].action_id = action_id;
    meta.lfu_header_stack[0].data_field_1 = data_field_1;
}

action modify_flow(bit<3> table_id, bit<48> key_field_1, bit<3> action_id, bit<9> data_field_1) {
    meta.lfu_header_stack.push_front(1);
    meta.header_count = meta.header_count + 1;
    meta.lfu_header_stack[0].update_type = 1;
    meta.lfu_header_stack[0].table_id = table_id;
    meta.lfu_header_stack[0].key_field_1 = key_field_1;
    meta.lfu_header_stack[0].action_id = action_id;
    meta.lfu_header_stack[0].data_field_1 = data_field_1;
}

action delete_flow(bit<3> table_id, bit<48> key_field_1) {
    meta.lfu_header_stack.push_front(1);
    meta.header_count = meta.header_count + 1;
    meta.lfu_header_stack[0].update_type = 0;
    meta.lfu_header_stack[0].table_id = table_id;
    meta.lfu_header_stack[0].key_field_1 = key_field_1;
    meta.lfu_header_stack[0].action_id = 0;
    meta.lfu_header_stack[0].data_field_1 = 0;
}

    action drop() {
        mark_to_drop();
    }

    action learn() {
	add_flow(0, hdr.ethernet.srcAddr, 0, standard_metadata.ingress_port);
	add_flow(1, hdr.ethernet.srcAddr, 1, standard_metadata.ingress_port);
    }

    @name(".lfu_action_1") action mac_forward(egressSpec_t port) {
	standard_metadata.egress_spec = port;
    }
    
    @name(".lfu_table_0") table learning_table {
	key = {
	    hdr.ethernet.srcAddr: exact;
	}
	actions = {
	    NoAction;
	    learn;
	}
	default_action = learn();
    }

    @name(".lfu_table_1") table forwarding_table {
        key = {
            hdr.ethernet.dstAddr: exact;
        }
        actions = {
            mac_forward;
            drop;
        }
        size = 1024;
        default_action = drop();
    }

    apply {
        if (hdr.ethernet.isValid()) {
	    learning_table.apply();
            forwarding_table.apply();
        }
    }
}

/*************************************************************************
****************  E G R E S S   P R O C E S S I N G   *******************
*************************************************************************/

control MyEgress(inout headers hdr,
                 inout metadata meta,
                 inout standard_metadata_t standard_metadata) {
    apply {  }
}

/*************************************************************************
*************   C H E C K S U M    C O M P U T A T I O N   **************
*************************************************************************/

control MyComputeChecksum(inout headers  hdr, inout metadata meta) { 
   apply {  }
}

/*************************************************************************
***********************  D E P A R S E R  *******************************
*************************************************************************/

control MyDeparser(packet_out packet, in headers hdr) {
    apply {
        packet.emit(hdr.ethernet);
    }
}

/*************************************************************************
***********************  S W I T C H  *******************************
*************************************************************************/

V1Switch(
MyParser(),
MyVerifyChecksum(),
MyIngress(),
MyEgress(),
MyComputeChecksum(),
MyDeparser()
) main;
