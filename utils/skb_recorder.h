// struct skb_recorder {
//   struct mutex skb_map_locks[CORE_NUM];
//   struct sk_buff *skb_map[CORE_NUM][ENTRIES];
// };

// int record_skb(struct skb_recorder* recorder,int index, struct sk_buff* skb) {
//   int inner_id;

//   if (IS_ERR(skb)) {
//     return false;
//   }
//   skb = skb_share_check(skb, GFP_ATOMIC);
//   if (!skb)
//     return false;
//   mutex_lock(&recorder->skb_map_locks[index]);
//   for (inner_id = 0; inner_id < ENTRIES; inner_id++) {
//     if (recorder->skb_map[index][inner_id] == NULL) {
//       recorder->skb_map[index][inner_id] = skb;
//       break;
//     }
//   }
//   mutex_unlock(&recorder->skb_map_locks[index]);
//   return inner_id == ENTRIES ? -1 : index + inner_id * CORE_NUM;
// }

// struct sk_buff* remove_packet(struct ,u64 id) {
//   struct sk_buff* skb = NULL;
//   int index = id % CORE_NUM;
//   int inner_id = (id - index) / CORE_NUM;
  
//   if (inner_id >= MAP_NUM) {
//     return NULL;
//   }

//   mutex_lock(&skb_map_locks[index]);
//   skb = skb_map[index][inner_id];
//   skb_map[index][inner_id] = NULL;
//   mutex_unlock(&skb_map_locks[index]);

//   return skb;
// }