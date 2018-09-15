
header lfu_header_t {
    bit<1> valid;
    bit<2> update_type;
    bit<3> table_id;
    #ifdef KEY_FIELD_1
    bit<KEY_FIELD_1> key_field_1;
    bit<KEY_FIELD_1> key_mask_1;
    #endif
    #ifdef KEY_FIELD_2
    bit<KEY_FIELD_2> key_field_2;
    bit<KEY_FIELD_2> key_mask_2;
    #endif
    #ifdef KEY_FIELD_3
    bit<KEY_FIELD_3> key_field_3;
    bit<KEY_FIELD_3> key_mask_3;
    #endif
    #ifdef KEY_FIELD_4
    bit<KEY_FIELD_4> key_field_4;
    bit<KEY_FIELD_4> key_mask_4;
    #endif
    #ifdef KEY_FIELD_5
    bit<KEY_FIELD_5> key_field_5;
    bit<KEY_FIELD_5> key_mask_5;
    #endif
    bit<3> action_id;   
    #ifdef DATA_FIELD_1
    bit<DATA_FIELD_1> data_field_1;
    #endif
    #ifdef DATA_FIELD_2
    bit<DATA_FIELD_2> data_field_2;
    #endif
    #ifdef DATA_FIELD_3
    bit<DATA_FIELD_3> data_field_3;
    #endif
    #ifdef DATA_FIELD_4
    bit<DATA_FIELD_4> data_field_4;
    #endif
    #ifdef DATA_FIELD_5
    bit<DATA_FIELD_5> data_field_5;
    #endif
}

struct match_key_t {
    #ifdef KEY_FIELD_1
    bit<KEY_FIELD_1> key_field_1;
    bit<KEY_FIELD_1> key_mask_1;
    #endif
    #ifdef KEY_FIELD_2
    bit<KEY_FIELD_2> key_field_2;
    bit<KEY_FIELD_2> key_mask_2;
    #endif
    #ifdef KEY_FIELD_3
    bit<KEY_FIELD_3> key_field_3;
    bit<KEY_FIELD_3> key_mask_3;
    #endif
    #ifdef KEY_FIELD_4
    bit<KEY_FIELD_4> key_field_4;
    bit<KEY_FIELD_4> key_mask_4;
    #endif
    #ifdef KEY_FIELD_5
    bit<KEY_FIELD_5> key_field_5;
    bit<KEY_FIELD_5> key_mask_5;
    #endif
}

struct adata_t {
    #ifdef DATA_FIELD_1
    bit<DATA_FIELD_1> data_field_1;
    #endif
    #ifdef DATA_FIELD_2
    bit<DATA_FIELD_2> data_field_2;
    #endif
    #ifdef DATA_FIELD_3
    bit<DATA_FIELD_3> data_field_3;
    #endif
    #ifdef DATA_FIELD_4
    bit<DATA_FIELD_4> data_field_4;
    #endif
    #ifdef DATA_FIELD_5
    bit<DATA_FIELD_5> data_field_5;
    #endif
}




action fill_header(inout lfu_header_t lfu_header, bit<3> table_id, match_key_t match_key, bit<3> action_id, adata_t adata) {
    lfu_header.valid = 1;
    lfu_header.table_id = table_id;
    #ifdef KEY_FIELD_1
    lfu_header.key_field_1 = match_key.key_field_1;
    lfu_header.key_mask_1 = match_key.key_mask_1;
    #endif
    #ifdef KEY_FIELD_2
    lfu_header.key_field_2 = match_key.key_field_2;
    lfu_header.key_mask_2 = match_key.key_mask_2;
    #endif
    #ifdef KEY_FIELD_3
    lfu_header.key_field_3 = match_key.key_field_3;
    lfu_header.key_mask_3 = match_key.key_mask_3;
    #endif
    #ifdef KEY_FIELD_4
    lfu_header.key_field_4 = match_key.key_field_4;
    lfu_header.key_mask_4 = match_key.key_mask_4;
    #endif
    #ifdef KEY_FIELD_5
    lfu_header.key_field_5 = match_key.key_field_5;
    lfu_header.key_mask_5 = match_key.key_mask_5;
    #endif
    lfu_header.action_id = action_id;
    #ifdef DATA_FIELD_1
    lfu_header.data_field_1 = adata.data_field_1;
    #endif
    #ifdef DATA_FIELD_2
    lfu_header.data_field_2 = adata.data_field_2;
    #endif
    #ifdef DATA_FIELD_3
    lfu_header.data_field_3 = adata.data_field_3;
    #endif
    #ifdef DATA_FIELD_4
    lfu_header.data_field_4 = adata.data_field_4;
    #endif
    #ifdef DATA_FIELD_5
    lfu_header.data_field_5 = adata.data_field_5;
    #endif
}

action add_flow(inout lfu_header_t[5] lfu_hs, bit<3> table_id, match_key_t match_key, bit<3> action_id, adata_t adata) {
    lfu_hs.push_front(1);
    lfu_hs[0].setValid();
    lfu_hs[0].update_type = 0;
    fill_header(lfu_hs[0], table_id, match_key, action_id, adata);
}


action delete_flow(inout lfu_header_t[5] lfu_hs, bit<3> table_id, match_key_t match_key) {
    lfu_hs.push_front(1);
    lfu_hs[0].setValid();
    adata_t adata;
    lfu_hs[0].update_type = 1;
    fill_header(lfu_hs[0], table_id, match_key, 0, adata);
}

action modify_flow(inout lfu_header_t[5] lfu_hs, bit<3> table_id, match_key_t match_key, bit<3> action_id, adata_t adata) {
    lfu_hs.push_front(1);
    lfu_hs[0].setValid();
    lfu_hs[0].update_type = 2;
    fill_header(lfu_hs[0], table_id, match_key, action_id, adata);
}


