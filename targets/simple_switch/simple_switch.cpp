/* Copyright 2013-present Barefoot Networks, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Antonin Bas (antonin@barefootnetworks.com)
 *
 */

#include <bm/bm_sim/parser.h>
#include <bm/bm_sim/tables.h>
#include <bm/bm_sim/logger.h>

#include <unistd.h>

#include <iostream>
#include <fstream>
#include <string>

#include "simple_switch.h"
#include <regex> //matteo
#include <iterator> //matteo
#include <sys/time.h> //matteo

namespace {

struct hash_ex {
  uint32_t operator()(const char *buf, size_t s) const {
    const uint32_t p = 16777619;
    uint32_t hash = 2166136261;

    for (size_t i = 0; i < s; i++)
      hash = (hash ^ buf[i]) * p;

    hash += hash << 13;
    hash ^= hash >> 7;
    hash += hash << 3;
    hash ^= hash >> 17;
    hash += hash << 5;
    return static_cast<uint32_t>(hash);
  }
};

struct bmv2_hash {
  uint64_t operator()(const char *buf, size_t s) const {
    return bm::hash::xxh64(buf, s);
  }
};

}  // namespace

// if REGISTER_HASH calls placed in the anonymous namespace, some compiler can
// give an unused variable warning
REGISTER_HASH(hash_ex);
REGISTER_HASH(bmv2_hash);

extern int import_primitives();


SimpleSwitch::SimpleSwitch(int max_port, bool enable_swap)
  : Switch(enable_swap),
    max_port(max_port),
    input_buffer(1024),
#ifdef SSWITCH_PRIORITY_QUEUEING_ON
    egress_buffers(max_port, nb_egress_threads,
                   64, EgressThreadMapper(nb_egress_threads),
                   SSWITCH_PRIORITY_QUEUEING_NB_QUEUES),
#else
    egress_buffers(max_port, nb_egress_threads,
                   64, EgressThreadMapper(nb_egress_threads)),
#endif
    output_buffer(128),
    // cannot use std::bind because of a clang bug
    // https://stackoverflow.com/questions/32030141/is-this-incorrect-use-of-stdbind-or-a-compiler-bug
    my_transmit_fn([this](int port_num, const char *buffer, int len) {
        this->transmit_fn(port_num, buffer, len); }),
    pre(new McSimplePreLAG()),
    start(clock::now()) {
  add_component<McSimplePreLAG>(pre);

  add_required_field("standard_metadata", "ingress_port");
  add_required_field("standard_metadata", "packet_length");
  add_required_field("standard_metadata", "instance_type");
  add_required_field("standard_metadata", "egress_spec");
  add_required_field("standard_metadata", "clone_spec");
  add_required_field("standard_metadata", "egress_port");

  force_arith_field("standard_metadata", "ingress_port");
  force_arith_field("standard_metadata", "packet_length");
  force_arith_field("standard_metadata", "instance_type");
  force_arith_field("standard_metadata", "egress_spec");
  force_arith_field("standard_metadata", "clone_spec");

  force_arith_field("queueing_metadata", "enq_timestamp");
  force_arith_field("queueing_metadata", "enq_qdepth");
  force_arith_field("queueing_metadata", "deq_timedelta");
  force_arith_field("queueing_metadata", "deq_qdepth");
  force_arith_field("queueing_metadata", "qid");

  force_arith_field("intrinsic_metadata", "ingress_global_timestamp");
  force_arith_field("intrinsic_metadata", "lf_field_list");
  force_arith_field("intrinsic_metadata", "mcast_grp");
  force_arith_field("intrinsic_metadata", "resubmit_flag");
  force_arith_field("intrinsic_metadata", "egress_rid");
  force_arith_field("intrinsic_metadata", "recirculate_flag");

  import_primitives();
}

#define PACKET_LENGTH_REG_IDX 0

int
SimpleSwitch::receive_(int port_num, const char *buffer, int len) {
  static packet_id_t pkt_id = 0;

  // this is a good place to call this, because blocking this thread will not
  // block the processing of existing packet instances, which is a requirement
  if (do_swap() == 0) {
    check_queueing_metadata();
  }

  // we limit the packet buffer to original size + 512 bytes, which means we
  // cannot add more than 512 bytes of header data to the packet, which should
  // be more than enough
  auto packet = new_packet_ptr(port_num, pkt_id++, len,
                               bm::PacketBuffer(len + 512, buffer, len));

  BMELOG(packet_in, *packet);

  PHV *phv = packet->get_phv();
  // many current P4 programs assume this
  // it is also part of the original P4 spec
  phv->reset_metadata();

  // setting standard metadata

  phv->get_field("standard_metadata.ingress_port").set(port_num);
  // using packet register 0 to store length, this register will be updated for
  // each add_header / remove_header primitive call
  packet->set_register(PACKET_LENGTH_REG_IDX, len);
  phv->get_field("standard_metadata.packet_length").set(len);
  Field &f_instance_type = phv->get_field("standard_metadata.instance_type");
  f_instance_type.set(PKT_INSTANCE_TYPE_NORMAL);

  if (phv->has_field("intrinsic_metadata.ingress_global_timestamp")) {
    phv->get_field("intrinsic_metadata.ingress_global_timestamp")
        .set(get_ts().count());
  }

  input_buffer.push_front(std::move(packet));
  return 0;
}

void
SimpleSwitch::start_and_return_() {
  check_queueing_metadata();

  threads_.push_back(std::thread(&SimpleSwitch::ingress_thread, this));
  for (size_t i = 0; i < nb_egress_threads; i++) {
    threads_.push_back(std::thread(&SimpleSwitch::egress_thread, this, i));
  }
  threads_.push_back(std::thread(&SimpleSwitch::transmit_thread, this));
}

SimpleSwitch::~SimpleSwitch() {
  input_buffer.push_front(nullptr);
  for (size_t i = 0; i < nb_egress_threads; i++) {
#ifdef SSWITCH_PRIORITY_QUEUEING_ON
    egress_buffers.push_front(i, 0, nullptr);
#else
    egress_buffers.push_front(i, nullptr);
#endif
  }
  output_buffer.push_front(nullptr);
  for (auto& thread_ : threads_) {
    thread_.join();
  }
}

void
SimpleSwitch::reset_target_state_() {
  bm::Logger::get()->debug("Resetting simple_switch target-specific state");
  get_component<McSimplePreLAG>()->reset_state();
}

int
SimpleSwitch::set_egress_queue_depth(int port, const size_t depth_pkts) {
  egress_buffers.set_capacity(port, depth_pkts);
  return 0;
}

int
SimpleSwitch::set_all_egress_queue_depths(const size_t depth_pkts) {
  for (int i = 0; i < max_port; i++) {
    set_egress_queue_depth(i, depth_pkts);
  }
  return 0;
}

int
SimpleSwitch::set_egress_queue_rate(int port, const uint64_t rate_pps) {
  egress_buffers.set_rate(port, rate_pps);
  return 0;
}

int
SimpleSwitch::set_all_egress_queue_rates(const uint64_t rate_pps) {
  for (int i = 0; i < max_port; i++) {
    set_egress_queue_rate(i, rate_pps);
  }
  return 0;
}

uint64_t
SimpleSwitch::get_time_elapsed_us() const {
  return get_ts().count();
}

uint64_t
SimpleSwitch::get_time_since_epoch_us() const {
  auto tp = clock::now();
  return duration_cast<ts_res>(tp.time_since_epoch()).count();
}

void
SimpleSwitch::set_transmit_fn(TransmitFn fn) {
  my_transmit_fn = std::move(fn);
}

void
SimpleSwitch::transmit_thread() {
  while (1) {
    std::unique_ptr<Packet> packet;
    output_buffer.pop_back(&packet);
    if (packet == nullptr) break;
    BMELOG(packet_out, *packet);

    /*
    //matteo
    timeval time;
    gettimeofday(&time, NULL);
    long micros = (time.tv_sec * 1000000) + time.tv_usec;
    std::cout << "PKTLEAVTIME: " << micros << "\n";
    */

    BMLOG_DEBUG_PKT(*packet, "Transmitting packet of size {} out of port {}",
                    packet->get_data_size(), packet->get_egress_port());
    my_transmit_fn(packet->get_egress_port(),
                   packet->data(), packet->get_data_size());
  }
}

ts_res
SimpleSwitch::get_ts() const {
  return duration_cast<ts_res>(clock::now() - start);
}

void
SimpleSwitch::enqueue(int egress_port, std::unique_ptr<Packet> &&packet) {
    packet->set_egress_port(egress_port);

    PHV *phv = packet->get_phv();

    if (with_queueing_metadata) {
      phv->get_field("queueing_metadata.enq_timestamp").set(get_ts().count());
      phv->get_field("queueing_metadata.enq_qdepth")
          .set(egress_buffers.size(egress_port));
    }

#ifdef SSWITCH_PRIORITY_QUEUEING_ON
    size_t priority = phv->has_field(SSWITCH_PRIORITY_QUEUEING_SRC) ?
        phv->get_field(SSWITCH_PRIORITY_QUEUEING_SRC).get<size_t>() : 0u;
    if (priority >= SSWITCH_PRIORITY_QUEUEING_NB_QUEUES) {
      bm::Logger::get()->error("Priority out of range, dropping packet");
      return;
    }
    egress_buffers.push_front(
        egress_port, SSWITCH_PRIORITY_QUEUEING_NB_QUEUES - 1 - priority,
        std::move(packet));
#else
    egress_buffers.push_front(egress_port, std::move(packet));
#endif
}

// used for ingress cloning, resubmit
void
SimpleSwitch::copy_field_list_and_set_type(
    const std::unique_ptr<Packet> &packet,
    const std::unique_ptr<Packet> &packet_copy,
    PktInstanceType copy_type, p4object_id_t field_list_id) {
  PHV *phv_copy = packet_copy->get_phv();
  phv_copy->reset_metadata();
  FieldList *field_list = this->get_field_list(field_list_id);
  field_list->copy_fields_between_phvs(phv_copy, packet->get_phv());
  phv_copy->get_field("standard_metadata.instance_type").set(copy_type);
}

void
SimpleSwitch::check_queueing_metadata() {
  // TODO(antonin): add qid in required fields
  bool enq_timestamp_e = field_exists("queueing_metadata", "enq_timestamp");
  bool enq_qdepth_e = field_exists("queueing_metadata", "enq_qdepth");
  bool deq_timedelta_e = field_exists("queueing_metadata", "deq_timedelta");
  bool deq_qdepth_e = field_exists("queueing_metadata", "deq_qdepth");
  if (enq_timestamp_e || enq_qdepth_e || deq_timedelta_e || deq_qdepth_e) {
    if (enq_timestamp_e && enq_qdepth_e && deq_timedelta_e && deq_qdepth_e)
      with_queueing_metadata = true;
    else
      bm::Logger::get()->warn(
          "Your JSON input defines some but not all queueing metadata fields");
  }
}

//matteo
std::string SimpleSwitch::printError(MatchErrorCode rc) {
    std::string error;
    switch (rc) {
	case MatchErrorCode::SUCCESS : error = "SUCCESS"; break;
	case MatchErrorCode::TABLE_FULL : error = "TABLE_FULL"; break;
	case MatchErrorCode::INVALID_HANDLE : error = "INVALID_HANDLE"; break;
	case MatchErrorCode::EXPIRED_HANDLE : error = "EXPIRED_HANDLE"; break;
	case MatchErrorCode::COUNTERS_DISABLED : error = "COUNTERS_DISABLED"; break; 
	case MatchErrorCode::METERS_DISABLED : error = "METERS_DISABLED"; break; 
	case MatchErrorCode::AGEING_DISABLED : error = "AGEING_DISABLED"; break; 
	case MatchErrorCode::INVALID_TABLE_NAME : error = "INVALID_TABLE_NAME"; break; 
	case MatchErrorCode::INVALID_ACTION_NAME : error ="INVALID_ACTION_NAME"; break; 
	case MatchErrorCode::WRONG_TABLE_TYPE : error ="WRONG_TABLE_TYPE"; break; 
	case MatchErrorCode::INVALID_MBR_HANDLE : error ="INVALID_MBR_HANDLE"; break; 
	case MatchErrorCode::MBR_STILL_USED : error ="MBR_STILL_USED"; break; 
	case MatchErrorCode::MBR_ALREADY_IN_GRP : error ="MBR_ALREADY_IN_GRP"; break; 
	case MatchErrorCode::MBR_NOT_IN_GRP : error ="MBR_NOT_IN_GRP"; break; 
	case MatchErrorCode::INVALID_GRP_HANDLE : error ="INVALID_GRP_HANDLE"; break; 
	case MatchErrorCode::GRP_STILL_USED : error ="GRP_STILL_USED"; break; 
	case MatchErrorCode::EMPTY_GRP : error ="EMPTY_GRP"; break; 
	case MatchErrorCode::DUPLICATE_ENTRY : error = "DUPLICATE_ENTRY"; break; 
	case MatchErrorCode::BAD_MATCH_KEY : error ="BAD_MATCH_KEY"; break; 
	case MatchErrorCode::INVALID_METER_OPERATION : error = "INVALID_METER_OPERATION"; break; 
	case MatchErrorCode::DEFAULT_ACTION_IS_CONST : error ="DEFAULT_ACTION_IS_CONST"; break; 
	case MatchErrorCode::DEFAULT_ENTRY_IS_CONST : error ="DEFAULT_ENTRY_IS_CONST"; break; 
	case MatchErrorCode::NO_DEFAULT_ENTRY : error ="NO_DEFAULT_ENTRY"; break; 
	case MatchErrorCode::INVALID_ACTION_PROFILE_NAME : error = "INVALID_ACTION_PROFILE_NAME"; break; 
	case MatchErrorCode::NO_ACTION_PROFILE_SELECTION : error ="NO_ACTION_PROFILE_SELECTION"; break; 
	case MatchErrorCode::IMMUTABLE_TABLE_ENTRIES : error ="IMMUTABLE_TABLE_ENTRIES"; break; 
	case MatchErrorCode::BAD_ACTION_DATA : error = "BAD_ACTION_DATA"; break; 
	case MatchErrorCode::ERROR : error ="error"; break; 
	default  : error ="keine Ahnung"; break; 
    }
    return error;
}

//matteo
void
SimpleSwitch::apply_lfu_logic(Packet *packet) {
    
    PHV *phv = packet->get_phv();
    //auto &lfu_header_stack = phv->get_header_stack(get_header_stack_id_cfg("lfu_header_stack"));
    auto &lfu_header_stack = phv->get_header_stack(0); //TODO: get it by name
    int header_count = phv->get_field("scalars.metadata.header_count").get_int();
    for (int i = 0; i < header_count; i++) {
        auto &hdr = lfu_header_stack.at(i);

	//print header
	for (int j = 0; j < hdr.get_header_type().get_num_fields(); j++) {
	    std::cout << hdr.get_field_name(j) << ": " << hdr.get_field(j).get_int() << " | ";
        }
        std::cout << "\n";

	int update_type = hdr.get_field(0).get_int();
	//std::cout << "Update type (" << hdr.get_field_name(0) << "): " << update_type << "\n";
	int table_id = hdr.get_field(1).get_int();
	std::string table_name = "lfu_table_" + std::to_string(table_id);
	//std::cout << "Table name (" << hdr.get_field_name(1) << "): "<< table_name << "\n";
	std::vector<MatchKeyParam> match_key;
	
	
	int key_size = get_context(0)->get_key_size(table_name);
	std::cout << "Key size (" << table_name << "): "<< key_size << "\n";
	for (int j = 2; j < 2 + key_size ; j++) {
	    //std::cout << "Key field (" << hdr.get_field_name(j+2) << ")\n";
	    ByteContainer key_field = hdr.get_field(j).get_bytes();
	    //TODO: read match kind
	    match_key.emplace_back(MatchKeyParam::Type::EXACT, std::string(key_field.data(), key_field.size()));
	}

	std::string action_name;
        ActionData adata;
	if (update_type != 1) {
	    int action_offset = hdr.get_header_type().get_field_offset("action_id");
	    int action_id = hdr.get_field(action_offset).get_int();
	    action_name = action_id == 0 ? "NoAction" : "lfu_action_" + std::to_string(action_id);
	    std::cout << "Action name: " << action_name << "\n";


	    int num_params = get_context(0)->get_num_params_by_name(table_name, action_name);
	    std::cout << "Num params: " << num_params << "\n";

	    	    
	    for (int j = action_offset + 1; j < action_offset + 1+ num_params; j++) {
		
		adata.push_back_action_data(Data(hdr.get_field(j)));
		
	    }
	}

	MatchErrorCode rc;
        rc = MatchErrorCode::SUCCESS;
        uint32_t handle;
	if (update_type == 0) {

	    rc = mt_add_entry(0, table_name, match_key, action_name, adata, &handle);
	    std::cout << "Adding entry: " << SimpleSwitch::printError(rc) << "\n";
	} else {

	    MatchTable::Entry entry;
	    MatchErrorCode rc2 = mt_get_entry_from_key(0, table_name, match_key, &entry);
	    if (rc2 == MatchErrorCode::SUCCESS) {
	        if (update_type == 1) {
		    rc = mt_delete_entry(0, table_name, entry.handle);
		    std::cout << "Deleting entry: " << SimpleSwitch::printError(rc) << "\n";
	        } else {
		    rc = mt_modify_entry(0, table_name, entry.handle, action_name, adata);
		    std::cout << "Modifying: " << SimpleSwitch::printError(rc) << "\n";
	        }
	    } else {
		std::cout << "Failed to retrieve entry: " << SimpleSwitch::printError(rc2) << "\n";
	    }
	}
    }
}


void
SimpleSwitch::ingress_thread() {
  PHV *phv;

  while (1) {
    std::unique_ptr<Packet> packet;
    input_buffer.pop_back(&packet);
    if (packet == nullptr) break;

    // TODO(antonin): only update these if swapping actually happened?
    Parser *parser = this->get_parser("parser");
    Pipeline *ingress_mau = this->get_pipeline("ingress");

    phv = packet->get_phv();

    int ingress_port = packet->get_ingress_port();
    (void) ingress_port;

    /*
    //matteo
    timeval time;
    gettimeofday(&time, NULL);
    long micros = (time.tv_sec * 1000000) + time.tv_usec;
    std::cout << "PKTARRTIME: " << micros << "\n";
    */

    BMLOG_DEBUG_PKT(*packet, "Processing packet received on port {}",
                    ingress_port);

    /* This looks like it comes out of the blue. However this is needed for
       ingress cloning. The parser updates the buffer state (pops the parsed
       headers) to make the deparser's job easier (the same buffer is
       re-used). But for ingress cloning, the original packet is needed. This
       kind of looks hacky though. Maybe a better solution would be to have the
       parser leave the buffer unchanged, and move the pop logic to the
       deparser. TODO? */
    const Packet::buffer_state_t packet_in_state = packet->save_buffer_state();
    parser->parse(packet.get());

    ingress_mau->apply(packet.get());


    //matteo    
    apply_lfu_logic(packet.get());
    /*
    if (learn_action!=0) {
      std::vector<MatchKeyParam> match_key;
      std::vector<MatchKeyParam> match_key_event;
      std::vector<MatchKeyParam> match_key_mask;

      int update_fields = phv->get_header("userMetadata.update_scope").get_header_type().get_num_fields();
      for (int i = 0; i < update_fields -1 ; i++) { // -1 because there is an extra field called "$valid$"
	std::string field_name = phv->get_header("userMetadata.update_scope").get_header_type().get_field_name(i);
	if (field_name != "_padding") {
	  std::regex e("__");
	  field_name = std::regex_replace(field_name, e, ".");
          ByteContainer key = phv->get_field(field_name).get_bytes();
	  match_key.emplace_back(MatchKeyParam::Type::LPM, std::string(key.data(), key.size()), 32); //TODO: use length
	  match_key_event.emplace_back(MatchKeyParam::Type::LPM, std::string(key.data(), key.size()), 32); //TODO: use length
	  match_key_mask.emplace_back(MatchKeyParam::Type::LPM, std::string(key.data(), key.size()), 32); //TODO: use length
	}
      }

      int event_fields = phv->get_header("userMetadata.event").get_header_type().get_num_fields();
      std::cout << "event_fields: " << event_fields << "\n";
      for (int i = 0; i < event_fields -1 ; i++) { // -1 because there is an extra field called "$valid$"
	std::string field_name = phv->get_header("userMetadata.event").get_header_type().get_field_name(i);
	if (field_name != "_padding") { //TODO: use regex
	  std::regex e("__");
	  field_name = std::regex_replace(field_name, e, ".");
	  ByteContainer mask1("0x01FF");
	  ByteContainer mask2("0x0000");
	  ByteContainer key = phv->get_field(field_name).get_bytes();
	  match_key_event.emplace_back(MatchKeyParam::Type::TERNARY, std::string(key.data(),key.size()),std::string(mask1.data(),mask1.size()));
	  match_key_mask.emplace_back(MatchKeyParam::Type::TERNARY,std::string(key.data(),key.size()),std::string(mask2.data(),mask2.size()));
	  //TODO:use length
	}
      }

      ActionData adata1, adata2, adata3;
      int params_fields = phv->get_header("userMetadata.action_params").get_header_type().get_num_fields();
      for (int i = 0; i < params_fields -1 ; i++) { // -1 because there is an extra field called "$valid$"
	std::string field_name = phv->get_header("userMetadata.action_params").get_header_type().get_field_name(i);
	if (!std::regex_match(field_name, std::regex("_padding(.*)"))) { 
	  std::regex e("__");
	  field_name = std::regex_replace(field_name, std::regex("__"), ".");
          adata1.push_back_action_data(Data(phv->get_field(field_name)));
	}
      }
      uint32_t handle1, handle2, handle3;
      MatchErrorCode rc1, rc2, rc3;
      rc1 = rc2 = rc3 = MatchErrorCode::SUCCESS;
      
      if (learn_action==2) {
	rc1 = mt_add_entry(0, "lfu_table", match_key, "lfu_action", adata1, &handle1);
	std::cout << "adding lfu_table entry: " << SimpleSwitch::printError(rc1) << "\n";
	rc2 = mt_add_entry(0, "lfu_policy", match_key_event, "not_learn", adata2, &handle2, 2);
	std::cout << "adding event entry: " << SimpleSwitch::printError(rc2) << "\n";
	adata3.push_back_action_data(Data(handle2));
        if (event_fields > 1) {
	  rc3 = mt_add_entry(0, "lfu_policy", match_key_mask, "learn_modify", adata3, &handle3, 3);
	  std::cout << "adding masked entry: " << SimpleSwitch::printError(rc3) << "\n";
	} else {
	  std::cout << "Update part not required\n";
	}
      } else if (learn_action==1) {
	MatchErrorCode rc4, rc5, rc6;
	rc4 = rc5 = MatchErrorCode::SUCCESS;
	MatchTable::Entry entry1, entry2;
	rc4 = mt_get_entry_from_key(0, "lfu_table", match_key, &entry1);
	std::cout << "getting lfu_table entry: " << SimpleSwitch::printError(rc4) << "\n";
	rc1 = mt_modify_entry(0, "lfu_table", entry1.handle, "lfu_action", adata1);
	std::cout << "modifying lfu_table entry: " << SimpleSwitch::printError(rc1) << "\n";
	uint32_t old_entry = phv->get_field("scalars.metadata.action_handle").get_uint();
	rc3 = mt_delete_entry(0, "lfu_policy", old_entry);
	std::cout << "deleting event entry: " << SimpleSwitch::printError(rc3) << "\n";
	rc2 = mt_add_entry(0, "lfu_policy", match_key_event, "not_learn", adata2, &handle2, 2);
	std::cout << "adding event entry: " << SimpleSwitch::printError(rc2) << "\n";
	rc6 = mt_get_entry_from_key(0, "lfu_policy", match_key_mask, &entry2);
	std::cout << "getting masked entry: " << SimpleSwitch::printError(rc6) << "\n";	
	adata3.push_back_action_data(Data(handle2));
	rc5 = mt_modify_entry(0, "lfu_policy", entry2.handle, "learn_modify", adata3);
	std::cout << "modifying masked entry: " << SimpleSwitch::printError(rc5) << "\n";
      } else {
	//error
      }


    }
	
    */
    packet->reset_exit();

    Field &f_egress_spec = phv->get_field("standard_metadata.egress_spec");
    int egress_spec = f_egress_spec.get_int();

    Field &f_clone_spec = phv->get_field("standard_metadata.clone_spec");
    unsigned int clone_spec = f_clone_spec.get_uint();

    int learn_id = 0;
    unsigned int mgid = 0u;

    if (phv->has_field("intrinsic_metadata.lf_field_list")) {
      Field &f_learn_id = phv->get_field("intrinsic_metadata.lf_field_list");
      learn_id = f_learn_id.get_int();
    }

    // detect mcast support, if this is true we assume that other fields needed
    // for mcast are also defined
    if (phv->has_field("intrinsic_metadata.mcast_grp")) {
      Field &f_mgid = phv->get_field("intrinsic_metadata.mcast_grp");
      mgid = f_mgid.get_uint();
    }

    int egress_port;

    // INGRESS CLONING
    if (clone_spec) {
      BMLOG_DEBUG_PKT(*packet, "Cloning packet at ingress");
      egress_port = get_mirroring_mapping(clone_spec & 0xFFFF);
      f_clone_spec.set(0);
      if (egress_port >= 0) {
        const Packet::buffer_state_t packet_out_state =
            packet->save_buffer_state();
        packet->restore_buffer_state(packet_in_state);
        p4object_id_t field_list_id = clone_spec >> 16;
        std::unique_ptr<Packet> packet_copy = packet->clone_no_phv_ptr();
        // we need to parse again
        // the alternative would be to pay the (huge) price of PHV copy for
        // every ingress packet
        parser->parse(packet_copy.get());
        copy_field_list_and_set_type(packet, packet_copy,
                                     PKT_INSTANCE_TYPE_INGRESS_CLONE,
                                     field_list_id);
        enqueue(egress_port, std::move(packet_copy));
        packet->restore_buffer_state(packet_out_state);
      }
    }

    // LEARNING
    if (learn_id > 0) {
      get_learn_engine()->learn(learn_id, *packet.get());
    }

    // RESUBMIT
    if (phv->has_field("intrinsic_metadata.resubmit_flag")) {
      Field &f_resubmit = phv->get_field("intrinsic_metadata.resubmit_flag");
      if (f_resubmit.get_int()) {
        BMLOG_DEBUG_PKT(*packet, "Resubmitting packet");
        // get the packet ready for being parsed again at the beginning of
        // ingress
        packet->restore_buffer_state(packet_in_state);
        p4object_id_t field_list_id = f_resubmit.get_int();
        f_resubmit.set(0);
        // TODO(antonin): a copy is not needed here, but I don't yet have an
        // optimized way of doing this
        std::unique_ptr<Packet> packet_copy = packet->clone_no_phv_ptr();
        copy_field_list_and_set_type(packet, packet_copy,
                                     PKT_INSTANCE_TYPE_RESUBMIT,
                                     field_list_id);
        input_buffer.push_front(std::move(packet_copy));
        continue;
      }
    }

    Field &f_instance_type = phv->get_field("standard_metadata.instance_type");

    // MULTICAST
    int instance_type = f_instance_type.get_int();
    if (mgid != 0) {
      BMLOG_DEBUG_PKT(*packet, "Multicast requested for packet");
      Field &f_rid = phv->get_field("intrinsic_metadata.egress_rid");
      const auto pre_out = pre->replicate({mgid});
      auto packet_size = packet->get_register(PACKET_LENGTH_REG_IDX);
      for (const auto &out : pre_out) {
        egress_port = out.egress_port;
        // if (ingress_port == egress_port) continue; // pruning
        BMLOG_DEBUG_PKT(*packet, "Replicating packet on port {}", egress_port);
        f_rid.set(out.rid);
        f_instance_type.set(PKT_INSTANCE_TYPE_REPLICATION);
        std::unique_ptr<Packet> packet_copy = packet->clone_with_phv_ptr();
        packet_copy->set_register(PACKET_LENGTH_REG_IDX, packet_size);
        enqueue(egress_port, std::move(packet_copy));
      }
      f_instance_type.set(instance_type);

      // when doing multicast, we discard the original packet
      continue;
    }

    egress_port = egress_spec;
    BMLOG_DEBUG_PKT(*packet, "Egress port is {}", egress_port);

    if (egress_port == 511) {  // drop packet

      /*
      //matteo
      timeval time;
      gettimeofday(&time, NULL);
      long micros = (time.tv_sec * 1000000) + time.tv_usec;
      std::cout << "PKTLEAVTIME: " << micros << "\n";
      */

      BMLOG_DEBUG_PKT(*packet, "Dropping packet at the end of ingress");
      continue;
    }

    enqueue(egress_port, std::move(packet));
  }
}

void
SimpleSwitch::egress_thread(size_t worker_id) {
  PHV *phv;

  while (1) {
    std::unique_ptr<Packet> packet;
    size_t port;
#ifdef SSWITCH_PRIORITY_QUEUEING_ON
    size_t priority;
    egress_buffers.pop_back(worker_id, &port, &priority, &packet);
#else
    egress_buffers.pop_back(worker_id, &port, &packet);
#endif
    if (packet == nullptr) break;

    Deparser *deparser = this->get_deparser("deparser");
    Pipeline *egress_mau = this->get_pipeline("egress");

    phv = packet->get_phv();

    if (with_queueing_metadata) {
      auto enq_timestamp =
          phv->get_field("queueing_metadata.enq_timestamp").get<ts_res::rep>();
      phv->get_field("queueing_metadata.deq_timedelta").set(
          get_ts().count() - enq_timestamp);
      phv->get_field("queueing_metadata.deq_qdepth").set(
          egress_buffers.size(port));
      if (phv->has_field("queueing_metadata.qid")) {
        auto &qid_f = phv->get_field("queueing_metadata.qid");
#ifdef SSWITCH_PRIORITY_QUEUEING_ON
        qid_f.set(SSWITCH_PRIORITY_QUEUEING_NB_QUEUES - 1 - priority);
#else
        qid_f.set(0);
#endif
      }
    }

    phv->get_field("standard_metadata.egress_port").set(port);

    Field &f_egress_spec = phv->get_field("standard_metadata.egress_spec");
    f_egress_spec.set(0);

    phv->get_field("standard_metadata.packet_length").set(
        packet->get_register(PACKET_LENGTH_REG_IDX));

    egress_mau->apply(packet.get());

    Field &f_clone_spec = phv->get_field("standard_metadata.clone_spec");
    unsigned int clone_spec = f_clone_spec.get_uint();

    // EGRESS CLONING
    if (clone_spec) {
      BMLOG_DEBUG_PKT(*packet, "Cloning packet at egress");
      int egress_port = get_mirroring_mapping(clone_spec & 0xFFFF);
      if (egress_port >= 0) {
        f_clone_spec.set(0);
        p4object_id_t field_list_id = clone_spec >> 16;
        std::unique_ptr<Packet> packet_copy =
            packet->clone_with_phv_reset_metadata_ptr();
        PHV *phv_copy = packet_copy->get_phv();
        FieldList *field_list = this->get_field_list(field_list_id);
        field_list->copy_fields_between_phvs(phv_copy, phv);
        phv_copy->get_field("standard_metadata.instance_type")
            .set(PKT_INSTANCE_TYPE_EGRESS_CLONE);
        enqueue(egress_port, std::move(packet_copy));
      }
    }

    // TODO(antonin): should not be done like this in egress pipeline
    int egress_spec = f_egress_spec.get_int();
    if (egress_spec == 511) {  // drop packet
      BMLOG_DEBUG_PKT(*packet, "Dropping packet at the end of egress");
      continue;
    }

    deparser->deparse(packet.get());

    // RECIRCULATE
    if (phv->has_field("intrinsic_metadata.recirculate_flag")) {
      Field &f_recirc = phv->get_field("intrinsic_metadata.recirculate_flag");
      if (f_recirc.get_int()) {
        BMLOG_DEBUG_PKT(*packet, "Recirculating packet");
        p4object_id_t field_list_id = f_recirc.get_int();
        f_recirc.set(0);
        FieldList *field_list = this->get_field_list(field_list_id);
        // TODO(antonin): just like for resubmit, there is no need for a copy
        // here, but it is more convenient for this first prototype
        std::unique_ptr<Packet> packet_copy = packet->clone_no_phv_ptr();
        PHV *phv_copy = packet_copy->get_phv();
        phv_copy->reset_metadata();
        field_list->copy_fields_between_phvs(phv_copy, phv);
        phv_copy->get_field("standard_metadata.instance_type")
            .set(PKT_INSTANCE_TYPE_RECIRC);
        size_t packet_size = packet_copy->get_data_size();
        packet_copy->set_register(PACKET_LENGTH_REG_IDX, packet_size);
        phv_copy->get_field("standard_metadata.packet_length").set(packet_size);
        input_buffer.push_front(std::move(packet_copy));
        continue;
      }
    }

    output_buffer.push_front(std::move(packet));
  }
}
