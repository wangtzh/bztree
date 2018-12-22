// Copyright (c) Simon Fraser University
//
// Authors:
// Tianzheng Wang <tzwang@sfu.ca>
// Xiangpeng Hao <xiangpeng_hao@sfu.ca>

#include <algorithm>
#include <iostream>
#include <string>

#include "bztree.h"

namespace bztree {

// Create an internal node with a new key and associated child pointers inserted
// based on an existing internal node
InternalNode *InternalNode::New(InternalNode *src_node,
                                const char *key,
                                uint32_t key_size,
                                uint64_t left_child_addr,
                                uint64_t right_child_addr) {
  // FIXME(tzwang): use a better allocator
  uint32_t alloc_size = src_node->GetHeader()->size +
                        RecordMetadata::PadKeyLength(key_size) +
                        sizeof(right_child_addr) + sizeof(RecordMetadata);
  InternalNode *node = reinterpret_cast<InternalNode *>(malloc(alloc_size));
  memset(node, 0, alloc_size);
  new (node) InternalNode(alloc_size, src_node, key, key_size, left_child_addr, right_child_addr);
  return node;
}

// Create an internal node with a single separator key and two pointers
InternalNode *InternalNode::New(const char *key,
                                uint32_t key_size,
                                uint64_t left_child_addr,
                                uint64_t right_child_addr) {
  uint32_t alloc_size = sizeof(InternalNode) +
                        RecordMetadata::PadKeyLength(key_size) +
                        sizeof(left_child_addr) +
                        sizeof(right_child_addr) +
                        sizeof(RecordMetadata) * 2;
  InternalNode *node = reinterpret_cast<InternalNode *>(malloc(alloc_size));
  memset(node, 0, alloc_size);
  new (node) InternalNode(alloc_size, key, key_size, left_child_addr, right_child_addr);
  return node;
}

// Create an internal node with keys and pointers in the provided range from an
// existing source node
InternalNode *InternalNode::New(InternalNode *src_node,
                                uint32_t begin_meta_idx, uint32_t nr_records,
                                const char *key, uint32_t key_size,
                                uint64_t left_child_addr, uint64_t right_child_addr) {
  // Figure out how large the new node will be
  uint32_t alloc_size = sizeof(InternalNode);
  for (uint32_t i = begin_meta_idx; i < begin_meta_idx + nr_records; ++i) {
    RecordMetadata meta = src_node->GetMetadata(i);
    alloc_size += meta.GetTotalLength();
    alloc_size += sizeof(RecordMetadata);
  }

  // Add the new key, if provided
  if (key) {
    LOG_IF(FATAL, key_size == 0);
    alloc_size +=
      (RecordMetadata::PadKeyLength(key_size) + sizeof(uint64_t) + sizeof(RecordMetadata));
  }

  InternalNode *node = reinterpret_cast<InternalNode *>(malloc(alloc_size));
  memset(node, 0, alloc_size);
  new (node) InternalNode(alloc_size, src_node, begin_meta_idx, nr_records,
                          key, key_size, left_child_addr, right_child_addr);
}

InternalNode::InternalNode(uint32_t node_size, InternalNode *src_node,
                           uint32_t begin_meta_idx, uint32_t nr_records,
                           const char *key, uint32_t key_size,
                           uint64_t left_child_addr, uint64_t right_child_addr)
    : BaseNode(false, node_size) {
  for (uint32_t i = begin_meta_idx; i < begin_meta_idx + nr_records; ++i) {
    RecordMetadata meta = src_node->GetMetadata(i);
    // WIP(tzwang): add details
  }
}

InternalNode::InternalNode(uint32_t node_size,
                           const char *key,
                           const uint16_t key_size,
                           uint64_t left_child_addr,
                           uint64_t right_child_addr)
    : BaseNode(false, node_size) {
  // Initialize a new internal node with one key only
  header.sorted_count = 2;  // Includes the null dummy key

  // Fill in left child address, with an empty key
  uint64_t offset = node_size - sizeof(left_child_addr);
  record_metadata[0].FinalizeForInsert(offset, 0, sizeof(left_child_addr));
  char *ptr = reinterpret_cast<char *>(this) + offset;
  memcpy(ptr, &left_child_addr, sizeof(left_child_addr));

  // Fill in right child address, with the separator key
  auto padded_key_size = RecordMetadata::PadKeyLength(key_size);
  auto total_len = padded_key_size + sizeof(right_child_addr);
  offset -= total_len;
  record_metadata[1].FinalizeForInsert(offset, key_size, total_len);
  ptr = reinterpret_cast<char *>(this) + offset;
  memcpy(ptr, key, key_size);
  memcpy(ptr + padded_key_size, &right_child_addr, sizeof(right_child_addr));

  assert((uint64_t)ptr == (uint64_t)this + sizeof(*this) + 2 * sizeof(RecordMetadata));
}

InternalNode::InternalNode(uint32_t node_size,
                           InternalNode *src_node,
                           const char *key,
                           const uint16_t key_size,
                           uint64_t left_child_addr,
                           uint64_t right_child_addr)
    : BaseNode(false, node_size) {
  LOG_IF(FATAL, !src_node);
  auto padded_key_size = RecordMetadata::PadKeyLength(key_size);

  uint64_t offset = node_size;
  bool inserted_new = false;
  for (uint32_t i = 0; i < src_node->GetHeader()->sorted_count; ++i) {
    RecordMetadata meta = src_node->GetMetadata(i);
    uint64_t m_payload = 0;
    char *m_key;
    src_node->GetRecord(meta, &m_key, &m_payload);
    auto m_key_size = meta.GetKeyLength();

    if (inserted_new) {
      // New key already inserted, so directly insert the key from src node
      offset -= (meta.GetTotalLength());
      meta.FinalizeForInsert(offset, meta.GetKeyLength(), meta.GetTotalLength());
      record_metadata[i + 1] = meta;

      memcpy(reinterpret_cast<char *>(this) + offset, m_key, meta.GetTotalLength());
    } else {
      // Compare the two keys to see which one to insert (first)
      int cmp = memcmp(m_key, key, std::min<uint16_t>(m_key_size, key_size));
      LOG_IF(FATAL, cmp == 0 && key_size == m_key_size);

      if (cmp > 0) {
        RecordMetadata new_meta;
        offset -= (padded_key_size + sizeof(left_child_addr));
        new_meta.FinalizeForInsert(offset, key_size, sizeof(left_child_addr));
        record_metadata[i] = new_meta;

        // Modify the previous key's payload to left_child_addr
        auto &prev_meta = record_metadata[i - 1];
        memcpy(reinterpret_cast<char *>(this) +
                   prev_meta.GetOffset() + prev_meta.GetPaddedKeyLength(),
               &left_child_addr, sizeof(left_child_addr));

        // Now the new separtor key itself
        memcpy(reinterpret_cast<char *>(this) + offset, key, new_meta.GetTotalLength());
        memcpy(reinterpret_cast<char *>(this) + offset + padded_key_size,
               &right_child_addr, sizeof(right_child_addr));

        offset -= (meta.GetTotalLength());
        meta.FinalizeForInsert(offset, meta.GetKeyLength(), meta.GetTotalLength());
        record_metadata[i + 1] = meta;
        memcpy(reinterpret_cast<char *>(this) + offset, m_key, meta.GetTotalLength());

        inserted_new = true;
      } else {
        record_metadata[i] = meta;
        offset -= (meta.GetTotalLength());
        memcpy(reinterpret_cast<char *>(this) + offset, m_key, meta.GetTotalLength());
      }
    }
  }
}

InternalNode *InternalNode::PrepareForSplit(Stack &stack,
                                            uint32_t split_threshold,
                                            const char *key,
                                            uint32_t key_size,
                                            uint64_t left_child_addr,
                                            uint64_t right_child_addr) {
  uint32_t data_size = header.size + key_size +
      sizeof(right_child_addr) + sizeof(RecordMetadata);
  uint32_t new_node_size = sizeof(InternalNode) + data_size;
  if (new_node_size > split_threshold) {
    // After adding a key and pointers the new node would be too large. This
    // means we are effectively 'moving up' the tree to do split
    // So now we split the node and generate two new internal nodes
    auto status = header.status;
    if (status.IsFrozen()) {
      // Maybe hit another SMO, the entire split op will abort
      return nullptr;
    }

    LOG_IF(FATAL, header.sorted_count < 2);
    uint32_t n_left = header.sorted_count >> 1;

    InternalNode *left = nullptr;
    InternalNode *right = nullptr;

    // Figure out where does the new key will go
    auto meta = GetMetadata(n_left - 1);
    char *last_key_left = GetKey(meta);
    int cmp = memcmp(key, last_key_left, std::min<uint32_t>(key_size, meta.GetKeyLength()));
    if (cmp < 0 || (cmp == 0 && key_size < meta.GetKeyLength())) {
      // Should go to left
      left = InternalNode::New(this, 0, n_left, key, key_size, left_child_addr, right_child_addr);
      right = InternalNode::New(this, 0, header.sorted_count - n_left, nullptr, 0, 0, 0);
    } else {
      left = InternalNode::New(this, 0, n_left, nullptr, 0, 0, 0);
      right = InternalNode::New(this, 0, header.sorted_count - n_left,
                                key, key_size, left_child_addr, right_child_addr);
    }
    auto *parent = stack.Top() ? stack.Pop()->node : nullptr;
    meta = record_metadata[n_left];
    if (parent) {
      // Need to insert into this parent, so create a new one
      return parent->PrepareForSplit(stack, split_threshold, GetKey(meta), meta.GetKeyLength(),
                                     (uint64_t)left, (uint64_t)right);
    } else {
      // New root node
      return InternalNode::New(GetKey(meta), meta.GetKeyLength(), (uint64_t)left, (uint64_t)right);
    }
  } else {
    return InternalNode::New(this, key, key_size, left_child_addr, right_child_addr);
  }
}

LeafNode *LeafNode::New() {
  // FIXME(tzwang): use a better allocator
  LeafNode *node = reinterpret_cast<LeafNode *>(malloc(kNodeSize));
  memset(node, 0, kNodeSize);
  new (node) LeafNode;
  return node;
}

void BaseNode::Dump() {
  std::cout << "-----------------------------" << std::endl;
  std::cout << " Dumping node: 0x" << this << (is_leaf ? " (leaf)" : " (internal)") << std::endl;
  std::cout << " Header:\n";
  if (is_leaf) {
    std::cout << " - free space: " << (reinterpret_cast<LeafNode *>(this))->GetFreeSpace()
              << std::endl;
  }
  std::cout << " - status: 0x" << std::hex << header.status.word << std::endl
            << "   (control = 0x" << (header.status.word & NodeHeader::StatusWord::kControlMask)
            << std::dec
            << ", frozen = " << header.status.IsFrozen()
            << ", block size = " << header.status.GetBlockSize()
            << ", delete size = " << header.status.GetDeleteSize()
            << ", record count = " << header.status.GetRecordCount() << ")\n"
            << " - sorted_count: " << header.sorted_count
            << std::endl;

  std::cout << " Record Metadata Array:" << std::endl;
  for (uint32_t i = 0; i < header.status.GetRecordCount(); ++i) {
    RecordMetadata meta = record_metadata[i];
    std::cout << " - record " << i << ": meta = 0x" << std::hex << meta.meta << std::endl;
    std::cout << std::hex;
    std::cout << "   (control = 0x" << (meta.meta & RecordMetadata::kControlMask)
              << std::dec
              << ", visible = " << meta.IsVisible()
              << ", offset = " << meta.GetOffset()
              << ", key length = " << meta.GetKeyLength()
              << ", total length = " << meta.GetTotalLength()
              << std::endl;
  }
}

void LeafNode::Dump() {
  BaseNode::Dump();
  std::cout << " Key-Payload Pairs:" << std::endl;
  for (uint32_t i = 0; i < header.status.GetRecordCount(); ++i) {
    RecordMetadata meta = record_metadata[i];
    uint64_t payload = 0;
    char *key;
    GetRecord(meta, &key, &payload);
    std::string keystr(key, key + meta.GetKeyLength());
    std::cout << " - record " << i << ": key = " << keystr
              << ", payload = " << payload << std::endl;
  }

  std::cout << "-----------------------------" << std::endl;
}

void InternalNode::Dump(bool dump_children) {
  BaseNode::Dump();
  std::cout << " Child pointers and separator keys:" << std::endl;
  assert(header.status.GetRecordCount() == 0);
  for (uint32_t i = 0; i < header.sorted_count; ++i) {
    auto &meta = record_metadata[i];
    assert((i == 0 && meta.GetKeyLength() == 0) || (i > 0 && meta.GetKeyLength() > 0));
    uint64_t right_child_addr = 0;
    char *key;
    GetRecord(meta, &key, &right_child_addr);
    if (key) {
      std::string keystr(key, key + meta.GetKeyLength());
      std::cout << " | " << keystr << " | ";
    }
    std::cout << std::hex << "0x" << right_child_addr << std::dec;
  }
  std::cout << std::endl;

  if (dump_children) {
    for (uint32_t i = 0; i < header.sorted_count; ++i) {
      uint64_t node_addr = *GetPayloadPtr(record_metadata[i]);
      BaseNode *node = reinterpret_cast<BaseNode *>(node_addr);
      if (node->IsLeaf()) {
        (reinterpret_cast<LeafNode *>(node_addr))->Dump();
      } else {
        (reinterpret_cast<InternalNode *>(node_addr))->Dump(true);
      }
    }
  }
}

ReturnCode LeafNode::Insert(uint32_t epoch, const char *key, uint16_t key_size, uint64_t payload,
                            pmwcas::DescriptorPool *pmwcas_pool) {
  retry:
  NodeHeader::StatusWord expected_status = header.status;

  // If frozon then retry
  if (expected_status.IsFrozen()) {
    return ReturnCode::NodeFrozen();
  }

  auto uniqueness = CheckUnique(key, key_size);
  if (uniqueness == Duplicate) {
    return ReturnCode::KeyExists();
  }

  // Now try to reserve space in the free space region using a PMwCAS. Two steps:
  // Step 1. Incrementing the record count and block size fields in [status]
  // Step 2. Flip the record metadata entry's high order bit and fill in global
  // epoch
  NodeHeader::StatusWord desired_status = expected_status;

  // Block size includes both key and payload sizes
  auto padded_key_size = RecordMetadata::PadKeyLength(key_size);
  auto total_size = padded_key_size + sizeof(payload);
  desired_status.PrepareForInsert(total_size);

  // Get the tentative metadata entry (again, make a local copy to work on it)
  RecordMetadata *meta_ptr = &record_metadata[expected_status.GetRecordCount()];
  RecordMetadata expected_meta = *meta_ptr;
  if (!expected_meta.IsVacant()) {
    goto retry;
  }

  RecordMetadata desired_meta;
  desired_meta.PrepareForInsert(epoch);

  // Now do the PMwCAS
  pmwcas::Descriptor *pd = pmwcas_pool->AllocateDescriptor();
  pd->AddEntry(&header.status.word, expected_status.word, desired_status.word);
  pd->AddEntry(&meta_ptr->meta, expected_meta.meta, desired_meta.meta);
  if (!pd->MwCAS()) {
    return ReturnCode::PMWCASFailure();
  }

  // Reserved space! Now copy data
  // The key size must be padded to 64bit
  uint64_t offset = kNodeSize - desired_status.GetBlockSize();
  char *ptr = &(reinterpret_cast<char *>(this))[offset];
  memcpy(ptr, key, key_size);
  memcpy(ptr + padded_key_size, &payload, sizeof(payload));
  // Flush the word
  pmwcas::NVRAM::Flush(total_size, ptr);

  if (uniqueness == ReCheck) {
    uniqueness = RecheckUnique(key, padded_key_size, expected_status.GetRecordCount());
    if (uniqueness == Duplicate) {
      memset(ptr, 0, key_size);
      memset(ptr + padded_key_size, 0, sizeof(payload));
      offset = 0;
    }
  }

  // Re-check if the node is frozen
  NodeHeader::StatusWord s = header.status;
  if (s.IsFrozen()) {
    return ReturnCode::NodeFrozen();
  } else {
    // Final step: make the new record visible, a 2-word PMwCAS:
    // 1. Metadata - set the visible bit and actual block offset
    // 2. Status word - set to the initial value read above (s) to detect
    // conflicting threads that are trying to set the frozen bit
    expected_meta = desired_meta;
    desired_meta.FinalizeForInsert(offset, key_size, total_size);

    pd = pmwcas_pool->AllocateDescriptor();
    pd->AddEntry(&header.status.word, s.word, s.word);
    pd->AddEntry(&meta_ptr->meta, expected_meta.meta, desired_meta.meta);
    return pd->MwCAS() ? ReturnCode::Ok() : ReturnCode::PMWCASFailure();
  }
}

LeafNode::Uniqueness LeafNode::CheckUnique(const char *key, uint32_t key_size) {
  auto record = SearchRecordMeta(key, key_size);
  if (record == nullptr) {
    return IsUnique;
  }
  if (!record->IsVisible()) {
    return ReCheck;
  }
  return Duplicate;
}

LeafNode::Uniqueness LeafNode::RecheckUnique(const char *key, uint32_t key_size, uint32_t end_pos) {
  retry:
  auto record = SearchRecordMeta(key, key_size, header.sorted_count, end_pos);
  if (record == nullptr) {
    return IsUnique;
  }
  if (record->IsInserting()) {
    goto retry;
  }
  return Duplicate;
}

ReturnCode LeafNode::Upsert(uint32_t epoch,
                            const char *key,
                            uint16_t key_size,
                            uint64_t payload,
                            pmwcas::DescriptorPool *pmwcas_pool) {
  retry:
  auto old_status = header.status;
  if (old_status.IsFrozen()) {
    return ReturnCode::NodeFrozen();
  }
  auto *meta_ptr = SearchRecordMeta(key, key_size);
  if (meta_ptr == nullptr) {
    auto insert_result = Insert(epoch, key, key_size, payload, pmwcas_pool);

    if (insert_result.IsPMWCASFailure()) {
      return Update(epoch, key, key_size, payload, pmwcas_pool);
    }
    return ReturnCode::Ok();
  } else if (meta_ptr->IsInserting()) {
    goto retry;
  } else {
    return Update(epoch, key, key_size, payload, pmwcas_pool);
  }
}

ReturnCode LeafNode::Update(uint32_t epoch,
                            const char *key,
                            uint16_t key_size,
                            uint64_t payload,
                            pmwcas::DescriptorPool *pmwcas_pool) {
  retry:
  auto old_status = header.status;
  if (old_status.IsFrozen()) {
    return ReturnCode::NodeFrozen();
  }

  auto *meta_ptr = SearchRecordMeta(key, key_size);
  if (meta_ptr == nullptr || !meta_ptr->IsVisible()) {
    return ReturnCode::NotFound();
  } else if (meta_ptr->IsInserting()) {
    goto retry;
  }
  auto old_meta_value = meta_ptr->meta;

  char *record_key;
  uint64_t record_payload;
  GetRecord(*meta_ptr, &record_key, &record_payload);
  if (payload == record_payload) {
    return ReturnCode::Ok();
  }

//  1. update the corresponding payload
//  2. make sure meta data is not changed
//  3. make sure status word is not changed
  auto pd = pmwcas_pool->AllocateDescriptor();
  pd->AddEntry(reinterpret_cast<uint64_t *>(record_key + meta_ptr->GetPaddedKeyLength()),
               record_payload, payload);
  pd->AddEntry(&meta_ptr->meta, old_meta_value, meta_ptr->meta);
  pd->AddEntry(&header.status.word, old_status.word, old_status.word);

  if (!pd->MwCAS()) {
    goto retry;
  }
  return ReturnCode::Ok();
}

RecordMetadata *BaseNode::SearchRecordMeta(const char *key,
                                           uint32_t key_size,
                                           uint32_t start_pos,
                                           uint32_t end_pos,
                                           bool check_concurrency) {
  if (start_pos < header.sorted_count) {
    // Binary search on sorted field
    int64_t first = start_pos;
    int64_t last = std::min<uint32_t>(end_pos, header.sorted_count - 1);
    int64_t middle;
    while (header.sorted_count != 0 && first <= last) {
      middle = (first + last) / 2;

      // Encountered a deleted record
      // Try to adjust the middle to left ones
      while (!record_metadata[middle].IsVisible() && first < middle) {
        middle -= 1;
      }

      // Every record on the left is deleted, now try right ones
      middle = (first + last) / 2;
      while (!record_metadata[middle].IsVisible() && middle < last) {
        middle += 1;
      }

      // Every record in the sorted field is deleted
      if (!record_metadata[middle].IsVisible()) {
        break;
      }

      uint64_t payload = 0;
      char *current_key;
      auto current = &(record_metadata[middle]);
      GetRecord(*current, &current_key, &payload);

      auto cmp_result = memcmp(key, current_key, current->GetKeyLength());
      if (cmp_result < 0) {
        last = middle - 1;
      } else if (cmp_result == 0 && key_size == current->GetKeyLength() && current->IsVisible()) {
        return current;
      } else {
        first = middle + 1;
      }
    }
  }
  if (end_pos > header.sorted_count) {
    // Linear search on unsorted field
    uint32_t linear_end = std::min<uint32_t>(header.status.GetRecordCount(), end_pos);
    for (uint32_t i = header.sorted_count; i < linear_end; i++) {
      auto current = &(record_metadata[i]);

      // Encountered an in-progress insert, recheck later
      if (current->IsInserting() && check_concurrency) {
        return &(record_metadata[i]);
      } else if (current->IsInserting() && !check_concurrency) {
        continue;
      }

      uint64_t payload = 0;
      char *current_key;
      GetRecord(*current, &current_key, &payload);
      if (current->IsVisible() &&
          key_size == current->GetKeyLength() &&
          strncmp(key, current_key, current->GetKeyLength()) == 0) {
        return current;
      }
    }
  }
  return nullptr;
}

ReturnCode LeafNode::Delete(const char *key,
                            uint16_t key_size,
                            pmwcas::DescriptorPool *pmwcas_pool) {
  NodeHeader::StatusWord old_status = header.status;
  if (old_status.IsFrozen()) {
    return ReturnCode::NodeFrozen();
  }

  retry:
  auto record_meta = SearchRecordMeta(key, key_size);
  if (record_meta == nullptr) {
    return ReturnCode::NotFound();
  } else if (record_meta->IsInserting()) {
    // FIXME(hao): not mentioned in the paper, should confirm later;
    goto retry;
  }

  auto old_meta = *record_meta;
  auto new_meta = *record_meta;
  new_meta.SetVisible(false);
  new_meta.SetOffset(0);

  auto new_status = old_status;
  auto old_delete_size = old_status.GetDeleteSize();
  new_status.SetDeleteSize(old_delete_size + record_meta->GetTotalLength());

  pmwcas::Descriptor *pd = pmwcas_pool->AllocateDescriptor();
  pd->AddEntry(&header.status.word, old_status.word, new_status.word);
  pd->AddEntry(&(record_meta->meta), old_meta.meta, new_meta.meta);
  if (!pd->MwCAS()) {
    goto retry;
  }
  return ReturnCode::Ok();
}
ReturnCode LeafNode::Read(const char *key, uint16_t key_size, uint64_t *payload) {
  auto meta = SearchRecordMeta(key, key_size, 0, (uint32_t) -1, false);
  if (meta == nullptr) {
    return ReturnCode::NotFound();
  }
  char *unused_key;
  if (GetRecord(*meta, &unused_key, payload)) {
    return ReturnCode::Ok();
  } else {
    return ReturnCode::NotFound();
  }
}

bool BaseNode::Freeze(pmwcas::DescriptorPool *pmwcas_pool) {
  NodeHeader::StatusWord expected = header.status;
  if (expected.IsFrozen()) {
    return false;
  }
  NodeHeader::StatusWord desired = expected;
  desired.Freeze();

  pmwcas::Descriptor *pd = pmwcas_pool->AllocateDescriptor();
  pd->AddEntry(&header.status.word, expected.word, desired.word);
  return pd->MwCAS();
}

LeafNode *LeafNode::Consolidate(pmwcas::DescriptorPool *pmwcas_pool) {
  // Freeze the node to prevent new modifications first
  if (!Freeze(pmwcas_pool)) {
    return nullptr;
  }

  thread_local std::vector<RecordMetadata> meta_vec;
  meta_vec.clear();
  SortMetadataByKey(meta_vec, true);

  // Allocate and populate a new node
  LeafNode *new_leaf = LeafNode::New();
  new_leaf->CopyFrom(this, meta_vec.begin(), meta_vec.end());

  pmwcas::NVRAM::Flush(kNodeSize, new_leaf);

  return new_leaf;
}

uint32_t LeafNode::SortMetadataByKey(std::vector<RecordMetadata> &vec, bool visible_only) {
  uint32_t total_size = 0;
  for (uint32_t i = 0; i < header.status.GetRecordCount(); ++i) {
    // TODO(tzwang): handle deletes
    if (record_metadata[i].IsVisible()) {
      auto meta = record_metadata[i];
      vec.emplace_back(meta);
      total_size += (meta.GetTotalLength());
    }
  }

  // Lambda for comparing two keys
  auto key_cmp = [this](RecordMetadata &m1, RecordMetadata &m2) -> bool {
    uint64_t l1 = m1.GetKeyLength();
    uint64_t l2 = m2.GetKeyLength();
    char *k1 = GetKey(m1);
    char *k2 = GetKey(m2);
    int cmp = memcmp(k1, k2, std::min<uint64_t>(l1, l2));
    if (cmp == 0) {
      return l1 < l2;
    }
    return cmp < 0;
  };

  std::sort(vec.begin(), vec.end(), key_cmp);
  return total_size;
}

void LeafNode::CopyFrom(LeafNode *node,
                        std::vector<RecordMetadata>::iterator begin_it,
                        std::vector<RecordMetadata>::iterator end_it) {
  // meta_vec is assumed to be in sorted order, insert records one by one
  uint64_t offset = kNodeSize;
  uint16_t nrecords = 0;
  for (auto it = begin_it; it != end_it; ++it) {
    auto meta = *it;
    uint64_t payload = 0;
    char *key;
    node->GetRecord(meta, &key, &payload);

    // Copy data
    uint64_t total_len = meta.GetTotalLength();
    offset -= total_len;
    char *ptr = &(reinterpret_cast<char *>(this))[offset];
    memcpy(ptr, key, total_len);

    // Setup new metadata
    RecordMetadata new_meta = meta;
    new_meta.FinalizeForInsert(offset, meta.GetKeyLength(), total_len);
    record_metadata[nrecords] = new_meta;
    ++nrecords;
  }
  // Finalize header stats
  header.status.SetBlockSize((uint32_t) (kNodeSize - offset));
  header.status.SetRecordCount(nrecords);
  header.sorted_count = nrecords;
}

ReturnCode InternalNode::Update(RecordMetadata meta,
                                InternalNode *old_child,
                                InternalNode *new_child,
                                pmwcas::DescriptorPool *pmwcas_pool) {
  auto status = header.status;
  if (status.IsFrozen()) {
    return ReturnCode::NodeFrozen();
  }

  // Conduct a 2-word PMwCAS to swap in the new child pointer while ensuring the
  // node isn't frozen by a concurrent thread
  pmwcas::Descriptor *pd = pmwcas_pool->AllocateDescriptor();
  pd->AddEntry(&header.status.word, status.word, status.word);
  pd->AddEntry(GetPayloadPtr(meta),
               reinterpret_cast<uint64_t>(old_child),
               reinterpret_cast<uint64_t>(new_child));
  if (pd->MwCAS()) {
    return ReturnCode::Ok();
  } else {
    return ReturnCode::PMWCASFailure();
  }
}

BaseNode *InternalNode::GetChild(const char *key, uint16_t key_size, RecordMetadata *out_meta) {
  // Keys in internal nodes are always sorted, visible
  int32_t left = 0, right = header.status.GetRecordCount() - 1;
  while (left <= right) {
    int32_t mid = (left + right) / 2;
    auto meta = record_metadata[mid];
    uint64_t meta_key_size = meta.GetKeyLength();
    uint64_t meta_payload = 0;
    char *meta_key;
    GetRecord(meta, &meta_key, &meta_payload);
    int cmp = memcmp(key, meta_key, std::min<uint64_t>(meta_key_size, key_size));
    if (cmp == 0) {
      if (meta_key_size == key_size) {
        // Key exists
        left = mid;
        break;
      }
    }
    if (cmp > 0) {
      right = mid - 1;
    } else {
      left = mid + 1;
    }
  }
  LOG_IF(FATAL, left < 0);

  auto meta = record_metadata[left];
  uint64_t meta_payload = 0;
  char *unused_key;
  GetRecord(meta, &unused_key, &meta_payload);
  if (out_meta) {
    *out_meta = meta;
  }
  return reinterpret_cast<BaseNode *>(meta_payload);
}

InternalNode *LeafNode::PrepareForSplit(uint32_t epoch, Stack &stack,
                                        uint32_t split_threshold,
                                        pmwcas::DescriptorPool *pmwcas_pool,
                                        LeafNode **left, LeafNode **right) {
  LOG_IF(FATAL, header.status.GetRecordCount() <= 2) << "Fewer than 2 records, can't split";
  // Set the frozen bit on the node to be split
  if (!Freeze(pmwcas_pool)) {
    return nullptr;
  }

  // Prepare new nodes: a parent node, a left leaf and a right leaf
  // FIXME(tzwang): not PM-safe, might leak
  *left = LeafNode::New();
  *right = LeafNode::New();

  thread_local std::vector<RecordMetadata> meta_vec;
  meta_vec.clear();
  uint32_t total_size = SortMetadataByKey(meta_vec, true);

  int32_t left_size = total_size / 2;
  uint32_t nleft = 0;
  for (uint32_t i = 0; i < meta_vec.size(); ++i) {
    auto &meta = meta_vec[i];
    ++nleft;
    left_size -= meta.GetTotalLength();
    if (left_size <= 0) {
      break;
    }
  }

  // TODO(tzwang): also put the new insert here to save some cycles
  auto left_end_it = meta_vec.begin() + nleft;
  (*left)->CopyFrom(this, meta_vec.begin(), left_end_it);
  (*right)->CopyFrom(this, left_end_it, meta_vec.end());

  LOG_IF(FATAL, nleft - 1 == 0);
  RecordMetadata separator_meta = meta_vec[nleft - 1];

  InternalNode *parent = stack.Top() ? stack.Pop()->node : nullptr;
  uint64_t unused = 0;
  char *key;
  GetRecord(separator_meta, &key, &unused);
  if (parent) {
    // Has a parent node. PrepareForSplit will see if we need to split this
    // parent node as well, and if so, return a new (possibly upper-level) parent
    // node that needs to be installed to its parent
    return parent->PrepareForSplit(stack, split_threshold, key, separator_meta.GetKeyLength(),
      reinterpret_cast<uint64_t>(*left), reinterpret_cast<uint64_t>(*right));
  } else {
    return InternalNode::New(key, separator_meta.GetKeyLength(),
      reinterpret_cast<uint64_t>(*left), reinterpret_cast<uint64_t>(*right));
  }
}

LeafNode *BzTree::TraverseToLeaf(Stack &stack, const char *key, uint64_t key_size) {
  BaseNode *node = root;
  InternalNode *parent = nullptr;
  assert(node);
  while (!node->IsLeaf()) {
    RecordMetadata meta;
    parent = reinterpret_cast<InternalNode *>(node);
    node = (reinterpret_cast<InternalNode *>(node))->GetChild(key, key_size, &meta);
    stack.Push(parent, meta);
  }
  return reinterpret_cast<LeafNode *>(node);
}

ReturnCode BzTree::Insert(const char *key, uint16_t key_size, uint64_t payload) {
  thread_local Stack stack;
  ReturnCode rc;
  pmwcas::EpochGuard guard(pmwcas_pool->GetEpoch());
  do {
    stack.Clear();
    pmwcas::EpochGuard guard(pmwcas_pool->GetEpoch());
    LeafNode *node = TraverseToLeaf(stack, key, key_size);

    // Check space to see if we need to split the node
    auto new_free_space = node->GetFreeSpace() -
                          RecordMetadata::PadKeyLength(key_size) - sizeof(payload);
    if (new_free_space <= 0) {
      // Should split
      LeafNode *left = nullptr;
      LeafNode *right = nullptr;
      InternalNode *parent =
        node->PrepareForSplit(epoch, stack, parameters.split_threshold, pmwcas_pool, &left, &right);
      if (!parent) {
        // TODO(tzwang): check memory leaks
        continue;
      }

      auto *top = stack.Pop();  // Pop out the immediate parent
      InternalNode *old_parent = nullptr;
      if (top) {
        old_parent = top->node;
      }
      top = stack.Top();
      if (top) {
        // There is a grand parent. We need to swap out the pointer to the old
        // parent and install the pointer to the new parent
        if (top->node->Update(top->meta, old_parent, parent, pmwcas_pool).IsNodeFrozen()) {
          continue;
        }
      } else {
        // No grand parent, so the new parent node will become the new root
        pmwcas::Descriptor *pd = pmwcas_pool->AllocateDescriptor();
        pd->AddEntry(reinterpret_cast<uint64_t *>(root),
                     reinterpret_cast<uint64_t>(old_parent),
                     reinterpret_cast<uint64_t>(parent));
        // TODO(tzwang): specify memory policy for new leaf nodes
        if (!pd->MwCAS()) {
          continue;
        }
      }
    } else {
      rc = node->Insert(epoch, key, key_size, payload, pmwcas_pool);
    }
  } while (!rc.IsOk() && !rc.IsKeyExists());
  return rc;
}

ReturnCode BzTree::Read(const char *key, uint16_t key_size, uint64_t *payload) {
  thread_local Stack stack;
  stack.Clear();
  LeafNode *node = TraverseToLeaf(stack, key, key_size);
  if (node == nullptr) {
    return ReturnCode::NotFound();
  }
  uint64_t tmp_payload;
  auto rc = node->Read(key, key_size, &tmp_payload);
  if (rc.IsOk()) {
    *payload = tmp_payload;
  }
  return rc;
}

ReturnCode BzTree::Update(const char *key, uint16_t key_size, uint64_t payload) {
  thread_local Stack stack;
  ReturnCode rc;
  pmwcas::EpochGuard guard(pmwcas_pool->GetEpoch());
  do {
    stack.Clear();
    LeafNode *node = TraverseToLeaf(stack, key, key_size);
    if (node == nullptr) {
      return ReturnCode::NotFound();
    }
    rc = node->Update(epoch, key, key_size, payload, pmwcas_pool);
  } while (rc.IsPMWCASFailure());
  return rc;
}

ReturnCode BzTree::Upsert(const char *key, uint16_t key_size, uint64_t payload) {
  thread_local Stack stack;
  stack.Clear();
  LeafNode *node = TraverseToLeaf(stack, key, key_size);
  if (node == nullptr) {
    return Insert(key, key_size, payload);
  }
  uint64_t tmp_payload;
  auto rc = node->Read(key, key_size, &tmp_payload);
  if (rc.IsNotFound()) {
    return Insert(key, key_size, payload);
  } else if (rc.IsOk()) {
    if (tmp_payload == payload) {
      return ReturnCode::Ok();
    }
    return Update(key, key_size, payload);
  }
}

ReturnCode BzTree::Delete(const char *key, uint16_t key_size) {
  thread_local Stack stack;
  ReturnCode rc;
  pmwcas::EpochGuard guard(pmwcas_pool->GetEpoch());
  do {
    stack.Clear();
    LeafNode *node = TraverseToLeaf(stack, key, key_size);
    if (node == nullptr) {
      return ReturnCode::NotFound();
    }
    rc = node->Delete(key, key_size, pmwcas_pool);
    auto new_block_size = node->GetHeader()->status.GetBlockSize();
    if (new_block_size <= parameters.merge_threshold) {
      // FIXME(hao): merge the nodes
    }
  } while (rc.IsNodeFrozen());
  return rc;
}

void BzTree::Dump() {
  std::cout << "-----------------------------" << std::endl;
  std::cout << "Dumping tree with root node: " << root << std::endl;
  // Traverse each level and dump each node
  if (root->IsLeaf()) {
    (reinterpret_cast<LeafNode *>(root))->Dump();
  } else {
    (reinterpret_cast<InternalNode *>(root))->Dump(true /* inlcude children */);
  }
}

}  // namespace bztree
