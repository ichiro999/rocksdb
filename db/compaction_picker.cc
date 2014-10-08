//  Copyright (c) 2013, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/compaction_picker.h"

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <inttypes.h>
#include <limits>
#include <string>
#include "db/filename.h"
#include "util/log_buffer.h"
#include "util/statistics.h"

namespace rocksdb {

uint64_t TotalCompensatedFileSize(const std::vector<FileMetaData*>& files) {
  uint64_t sum = 0;
  for (size_t i = 0; i < files.size() && files[i]; i++) {
    sum += files[i]->compensated_file_size;
  }
  return sum;
}

namespace {
// Determine compression type, based on user options, level of the output
// file and whether compression is disabled.
// If enable_compression is false, then compression is always disabled no
// matter what the values of the other two parameters are.
// Otherwise, the compression type is determined based on options and level.
CompressionType GetCompressionType(
    const ImmutableCFOptions& ioptions, int level,
    const bool enable_compression = true) {
  if (!enable_compression) {
    // disable compression
    return kNoCompression;
  }
  // If the use has specified a different compression level for each level,
  // then pick the compression for that level.
  if (!ioptions.compression_per_level.empty()) {
    const int n = ioptions.compression_per_level.size() - 1;
    // It is possible for level_ to be -1; in that case, we use level
    // 0's compression.  This occurs mostly in backwards compatibility
    // situations when the builder doesn't know what level the file
    // belongs to.  Likewise, if level is beyond the end of the
    // specified compression levels, use the last value.
    return ioptions.compression_per_level[std::max(0, std::min(level, n))];
  } else {
    return ioptions.compression;
  }
}


}  // anonymous namespace

CompactionPicker::CompactionPicker(const ImmutableCFOptions& ioptions,
                                   const InternalKeyComparator* icmp)
    : ioptions_(ioptions),
      compactions_in_progress_(ioptions_.num_levels),
      icmp_(icmp) {
}

CompactionPicker::~CompactionPicker() {}

void CompactionPicker::SizeBeingCompacted(std::vector<uint64_t>& sizes) {
  for (int level = 0; level < NumberLevels() - 1; level++) {
    uint64_t total = 0;
    for (auto c : compactions_in_progress_[level]) {
      assert(c->level() == level);
      for (int i = 0; i < c->num_input_files(0); i++) {
        total += c->input(0, i)->compensated_file_size;
      }
    }
    sizes[level] = total;
  }
}

// Clear all files to indicate that they are not being compacted
// Delete this compaction from the list of running compactions.
void CompactionPicker::ReleaseCompactionFiles(Compaction* c, Status status) {
  c->MarkFilesBeingCompacted(false);
  compactions_in_progress_[c->level()].erase(c);
  if (!status.ok()) {
    c->ResetNextCompactionIndex();
  }
}

Status CompactionPicker::GetCompactionInputsFromFileNumbers(
    autovector<CompactionInputFiles>* input_files,
    std::set<uint64_t>* input_set,
    const Version* version,
    const CompactionOptions& compact_options) const {
  if (input_set->size() == 0U) {
    return Status::InvalidArgument(
        "Compaction must include at least one file.");
  }
  assert(input_files);

  autovector<CompactionInputFiles> matched_input_files;
  matched_input_files.resize(version->NumberLevels());
  int first_non_empty_level = -1;
  int last_non_empty_level = -1;
  for (int level = 0; level < version->NumberLevels(); ++level) {
    for (auto file : version->files_[level]) {
      auto iter = input_set->find(file->fd.GetNumber());
      if (iter != input_set->end()) {
        matched_input_files[level].files.push_back(file);
        input_set->erase(iter);
        last_non_empty_level = level;
        if (first_non_empty_level == -1) {
          first_non_empty_level = level;
        }
      }
    }
  }

  if (!input_set->empty()) {
    std::string message(
        "Cannot find matched SST files for the following file numbers:");
    for (auto fn : *input_set) {
      message += " ";
      message += std::to_string(fn);
    }
    return Status::InvalidArgument(message);
  }

  for (int level = first_non_empty_level;
       level <= last_non_empty_level; ++level) {
    matched_input_files[level].level = level;
    input_files->emplace_back(std::move(matched_input_files[level]));
  }

  return Status::OK();
}


void CompactionPicker::GetRange(const std::vector<FileMetaData*>& inputs,
                                InternalKey* smallest, InternalKey* largest) {
  assert(!inputs.empty());
  smallest->Clear();
  largest->Clear();
  for (size_t i = 0; i < inputs.size(); i++) {
    FileMetaData* f = inputs[i];
    if (i == 0) {
      *smallest = f->smallest;
      *largest = f->largest;
    } else {
      if (icmp_->Compare(f->smallest, *smallest) < 0) {
        *smallest = f->smallest;
      }
      if (icmp_->Compare(f->largest, *largest) > 0) {
        *largest = f->largest;
      }
    }
  }
}

void CompactionPicker::GetRange(const std::vector<FileMetaData*>& inputs1,
                                const std::vector<FileMetaData*>& inputs2,
                                InternalKey* smallest, InternalKey* largest) {
  std::vector<FileMetaData*> all = inputs1;
  all.insert(all.end(), inputs2.begin(), inputs2.end());
  GetRange(all, smallest, largest);
}

bool CompactionPicker::ExpandWhileOverlapping(Compaction* c) {
  assert(c != nullptr);
  // If inputs are empty then there is nothing to expand.
  if (c->inputs_[0].empty()) {
    assert(c->inputs_[1].empty());
    // This isn't good compaction
    return false;
  }

  // GetOverlappingInputs will always do the right thing for level-0.
  // So we don't need to do any expansion if level == 0.
  if (c->level() == 0) {
    return true;
  }

  const int level = c->level();
  InternalKey smallest, largest;

  // Keep expanding c->inputs_[0] until we are sure that there is a
  // "clean cut" boundary between the files in input and the surrounding files.
  // This will ensure that no parts of a key are lost during compaction.
  int hint_index = -1;
  size_t old_size;
  do {
    old_size = c->inputs_[0].size();
    GetRange(c->inputs_[0].files, &smallest, &largest);
    c->inputs_[0].clear();
    c->input_version_->GetOverlappingInputs(
        level, &smallest, &largest, &c->inputs_[0].files,
        hint_index, &hint_index);
  } while(c->inputs_[0].size() > old_size);

  // Get the new range
  GetRange(c->inputs_[0].files, &smallest, &largest);

  // If, after the expansion, there are files that are already under
  // compaction, then we must drop/cancel this compaction.
  int parent_index = -1;
  if (c->inputs_[0].empty()) {
    Log(ioptions_.info_log,
        "[%s] ExpandWhileOverlapping() failure because zero input files",
        c->column_family_data()->GetName().c_str());
  }
  if (c->inputs_[0].empty() || FilesInCompaction(c->inputs_[0].files) ||
      (c->level() != c->output_level() &&
       ParentRangeInCompaction(c->input_version_, &smallest, &largest, level,
                               &parent_index))) {
    c->inputs_[0].clear();
    c->inputs_[1].clear();
    return false;
  }
  return true;
}

// Returns true if any one of specified files are being compacted
bool CompactionPicker::FilesInCompaction(
    const std::vector<FileMetaData*>& files) {
  for (unsigned int i = 0; i < files.size(); i++) {
    if (files[i]->being_compacted) {
      return true;
    }
  }
  return false;
}

// Returns true if any one of the parent files are being compacted
bool CompactionPicker::ParentRangeInCompaction(Version* version,
                                               const InternalKey* smallest,
                                               const InternalKey* largest,
                                               int level, int* parent_index) {
  std::vector<FileMetaData*> inputs;
  assert(level + 1 < NumberLevels());

  version->GetOverlappingInputs(level + 1, smallest, largest, &inputs,
                                *parent_index, parent_index);
  return FilesInCompaction(inputs);
}

// Populates the set of inputs from "level+1" that overlap with "level".
// Will also attempt to expand "level" if that doesn't expand "level+1"
// or cause "level" to include a file for compaction that has an overlapping
// user-key with another file.
void CompactionPicker::SetupOtherInputs(
    const MutableCFOptions& mutable_cf_options, Compaction* c) {
  // If inputs are empty, then there is nothing to expand.
  // If both input and output levels are the same, no need to consider
  // files at level "level+1"
  if (c->inputs_[0].empty() || c->level() == c->output_level()) {
    return;
  }

  const int level = c->level();
  InternalKey smallest, largest;

  // Get the range one last time.
  GetRange(c->inputs_[0].files, &smallest, &largest);

  // Populate the set of next-level files (inputs_[1]) to include in compaction
  c->input_version_->GetOverlappingInputs(
      level + 1, &smallest, &largest,
      &c->inputs_[1].files, c->parent_index_,
      &c->parent_index_);

  // Get entire range covered by compaction
  InternalKey all_start, all_limit;
  GetRange(c->inputs_[0].files, c->inputs_[1].files, &all_start, &all_limit);

  // See if we can further grow the number of inputs in "level" without
  // changing the number of "level+1" files we pick up. We also choose NOT
  // to expand if this would cause "level" to include some entries for some
  // user key, while excluding other entries for the same user key. This
  // can happen when one user key spans multiple files.
  if (!c->inputs_[1].empty()) {
    std::vector<FileMetaData*> expanded0;
    c->input_version_->GetOverlappingInputs(
        level, &all_start, &all_limit, &expanded0, c->base_index_, nullptr);
    const uint64_t inputs0_size = TotalCompensatedFileSize(c->inputs_[0].files);
    const uint64_t inputs1_size = TotalCompensatedFileSize(c->inputs_[1].files);
    const uint64_t expanded0_size = TotalCompensatedFileSize(expanded0);
    uint64_t limit = mutable_cf_options.ExpandedCompactionByteSizeLimit(level);
    if (expanded0.size() > c->inputs_[0].size() &&
        inputs1_size + expanded0_size < limit &&
        !FilesInCompaction(expanded0) &&
        !c->input_version_->HasOverlappingUserKey(&expanded0, level)) {
      InternalKey new_start, new_limit;
      GetRange(expanded0, &new_start, &new_limit);
      std::vector<FileMetaData*> expanded1;
      c->input_version_->GetOverlappingInputs(level + 1, &new_start, &new_limit,
                                              &expanded1, c->parent_index_,
                                              &c->parent_index_);
      if (expanded1.size() == c->inputs_[1].size() &&
          !FilesInCompaction(expanded1)) {
        Log(ioptions_.info_log,
            "[%s] Expanding@%d %zu+%zu (%" PRIu64 "+%" PRIu64
            " bytes) to %zu+%zu (%" PRIu64 "+%" PRIu64 "bytes)\n",
            c->column_family_data()->GetName().c_str(), level,
            c->inputs_[0].size(), c->inputs_[1].size(), inputs0_size,
            inputs1_size, expanded0.size(), expanded1.size(), expanded0_size,
            inputs1_size);
        smallest = new_start;
        largest = new_limit;
        c->inputs_[0].files = expanded0;
        c->inputs_[1].files = expanded1;
        GetRange(c->inputs_[0].files, c->inputs_[1].files,
                 &all_start, &all_limit);
      }
    }
  }

  // Compute the set of grandparent files that overlap this compaction
  // (parent == level+1; grandparent == level+2)
  if (level + 2 < NumberLevels()) {
    c->input_version_->GetOverlappingInputs(level + 2, &all_start, &all_limit,
                                            &c->grandparents_);
  }
}

Compaction* CompactionPicker::CompactRange(
    const MutableCFOptions& mutable_cf_options, Version* version,
    int input_level, int output_level, uint32_t output_path_id,
    const InternalKey* begin, const InternalKey* end,
    InternalKey** compaction_end) {
  // CompactionPickerFIFO has its own implementation of compact range
  assert(ioptions_.compaction_style != kCompactionStyleFIFO);

  std::vector<FileMetaData*> inputs;
  bool covering_the_whole_range = true;

  // All files are 'overlapping' in universal style compaction.
  // We have to compact the entire range in one shot.
  if (ioptions_.compaction_style == kCompactionStyleUniversal) {
    begin = nullptr;
    end = nullptr;
  }
  version->GetOverlappingInputs(input_level, begin, end, &inputs);
  if (inputs.empty()) {
    return nullptr;
  }

  // Avoid compacting too much in one shot in case the range is large.
  // But we cannot do this for level-0 since level-0 files can overlap
  // and we must not pick one file and drop another older file if the
  // two files overlap.
  if (input_level > 0) {
    const uint64_t limit = mutable_cf_options.MaxFileSizeForLevel(input_level) *
      mutable_cf_options.source_compaction_factor;
    uint64_t total = 0;
    for (size_t i = 0; i + 1 < inputs.size(); ++i) {
      uint64_t s = inputs[i]->compensated_file_size;
      total += s;
      if (total >= limit) {
        **compaction_end = inputs[i + 1]->smallest;
        covering_the_whole_range = false;
        inputs.resize(i + 1);
        break;
      }
    }
  }
  assert(output_path_id < static_cast<uint32_t>(ioptions_.db_paths.size()));
  Compaction* c = new Compaction(
      version, input_level, output_level,
      mutable_cf_options.MaxFileSizeForLevel(output_level),
      mutable_cf_options.MaxGrandParentOverlapBytes(input_level),
      output_path_id,
      GetCompressionType(ioptions_, output_level));

  c->inputs_[0].files = inputs;
  if (ExpandWhileOverlapping(c) == false) {
    delete c;
    Log(ioptions_.info_log,
        "[%s] Could not compact due to expansion failure.\n",
        version->cfd_->GetName().c_str());
    return nullptr;
  }

  SetupOtherInputs(mutable_cf_options, c);

  if (covering_the_whole_range) {
    *compaction_end = nullptr;
  }

  // These files that are to be manaully compacted do not trample
  // upon other files because manual compactions are processed when
  // the system has a max of 1 background compaction thread.
  c->MarkFilesBeingCompacted(true);

  // Is this compaction creating a file at the bottommost level
  c->SetupBottomMostLevel(true);

  c->is_manual_compaction_ = true;
  c->mutable_cf_options_ = mutable_cf_options;

  return c;
}

namespace {
// Test whether two files have overlapping key-ranges.
bool HaveOverlappingKeyRanges(
    const Comparator* c,
    const SstFileMetaData& a, const SstFileMetaData& b) {
  if (c->Compare(a.smallestkey, b.smallestkey) >= 0) {
    if (c->Compare(a.smallestkey, b.largestkey) <= 0) {
      // b.smallestkey <= a.smallestkey <= b.largestkey
      return true;
    }
  } else if (c->Compare(a.largestkey, b.smallestkey) >= 0) {
    // a.smallestkey < b.smallestkey <= a.largestkey
    return true;
  }
  if (c->Compare(a.largestkey, b.largestkey) <= 0) {
    if (c->Compare(a.largestkey, b.smallestkey) >= 0) {
      // b.smallestkey <= a.largestkey <= b.largestkey
      return true;
    }
  } else if (c->Compare(a.smallestkey, b.largestkey) <= 0) {
    // a.smallestkey <= b.largestkey < a.largestkey
    return true;
  }
  return false;
}
}  // namespace

Status CompactionPicker::SanitizeCompactionInputFilesForAllLevels(
      std::set<uint64_t>* input_files,
      const ColumnFamilyMetaData& cf_meta,
      const int output_level) const {
  auto& levels = cf_meta.levels;
  auto comparator = icmp_->user_comparator();

  // the smallest and largest key of the current compaction input
  std::string smallestkey;
  std::string largestkey;
  // a flag for initializing smallest and largest key
  bool is_first = false;
  const int kNotFound = -1;

  // For each level, it does the following things:
  // 1. Find the first and the last compaction input files
  //    in the current level.
  // 2. Include all files between the first and the last
  //    compaction input files.
  // 3. Update the compaction key-range.
  // 4. For all remaining levels, include files that have
  //    overlapping key-range with the compaction key-range.
  for (int l = 0; l <= output_level; ++l) {
    auto& current_files = levels[l].files;
    int first_included = static_cast<int>(current_files.size());
    int last_included = kNotFound;

    // identify the first and the last compaction input files
    // in the current level.
    for (size_t f = 0; f < current_files.size(); ++f) {
      if (input_files->find(current_files[f].file_number) !=
          input_files->end()) {
        first_included = std::min(first_included, static_cast<int>(f));
        last_included = std::max(last_included, static_cast<int>(f));
        if (is_first == false) {
          smallestkey = current_files[f].smallestkey;
          largestkey = current_files[f].largestkey;
          is_first = true;
        }
      }
    }
    if (last_included == kNotFound) {
      continue;
    }

    if (l != 0) {
      // expend the compaction input of the current level if it
      // has overlapping key-range with other non-compaction input
      // files in the same level.
      while (first_included > 0) {
        if (current_files[first_included - 1].largestkey <
            current_files[first_included].smallestkey) {
          break;
        }
        first_included--;
      }

      while (last_included < static_cast<int>(current_files.size()) - 1) {
        if (current_files[last_included + 1].smallestkey >
            current_files[last_included].largestkey) {
          break;
        }
        last_included++;
      }
    }

    // include all files between the first and the last compaction input files.
    for (int f = first_included; f <= last_included; ++f) {
      if (current_files[f].being_compacted) {
        return Status::Aborted(
            "File " + std::to_string(current_files[f].file_number) +
            " that has overlapping key range with one of the compaction "
            " input file is currently being compacted.");
      }
      input_files->insert(current_files[f].file_number);
    }

    // update smallest and largest key
    if (l == 0) {
      for (int f = first_included; f <= last_included; ++f) {
        if (comparator->Compare(
            smallestkey, current_files[f].smallestkey) > 0) {
          smallestkey = current_files[f].smallestkey;
        }
        if (comparator->Compare(
            largestkey, current_files[f].largestkey) < 0) {
          largestkey = current_files[f].largestkey;
        }
      }
    } else {
      if (comparator->Compare(
          smallestkey, current_files[first_included].smallestkey) > 0) {
        smallestkey = current_files[first_included].smallestkey;
      }
      if (comparator->Compare(
          largestkey, current_files[last_included].largestkey) < 0) {
        largestkey = current_files[last_included].largestkey;
      }
    }

    SstFileMetaData aggregated_file_meta(
        0, "", 0, 0, 0, 0,
        smallestkey,
        largestkey, false);

    // For all lower levels, include all overlapping files.
    for (int m = l + 1; m <= output_level; ++m) {
      for (auto& next_lv_file : levels[m].files) {
        if (HaveOverlappingKeyRanges(
            comparator, aggregated_file_meta, next_lv_file)) {
          if (next_lv_file.being_compacted) {
            return Status::Aborted(
                "File " + std::to_string(next_lv_file.file_number) +
                " that has overlapping key range with one of the compaction "
                " input file is currently being compacted.");
          }
          input_files->insert(next_lv_file.file_number);
        }
      }
    }
  }
  return Status::OK();
}

Status CompactionPicker::SanitizeCompactionInputFiles(
    std::set<uint64_t>* input_files,
    const ColumnFamilyMetaData& cf_meta,
    const int output_level) const {
  assert(static_cast<int>(cf_meta.levels.size()) - 1 ==
         cf_meta.levels[cf_meta.levels.size() - 1].level);
  if (output_level >= static_cast<int>(cf_meta.levels.size())) {
    return Status::InvalidArgument(
        "Output level for column family " + cf_meta.name +
        " must between [0, " +
        std::to_string(cf_meta.levels[cf_meta.levels.size() - 1].level) +
        "].");
  }

  if (output_level > MaxOutputLevel()) {
    return Status::InvalidArgument(
        "Exceed the maximum output level defined by "
        "the current compaction algorithm --- " +
            std::to_string(MaxOutputLevel()));
  }

  if (output_level < 0 && output_level != kDeletionCompaction) {
    return Status::InvalidArgument(
        "Output level cannot be negative.");
  }

  if (input_files->size() == 0) {
    return Status::InvalidArgument(
        "A compaction must contain at least one file.");
  }

  Status s = SanitizeCompactionInputFilesForAllLevels(
      input_files, cf_meta, output_level);

  if (!s.ok()) {
    return s;
  }

  // for all input files, check whether the file number matches
  // any currently-existing files.
  for (auto file_num : *input_files) {
    bool found = false;
    for (auto level_meta : cf_meta.levels) {
      for (auto file_meta : level_meta.files) {
        if (file_num == file_meta.file_number) {
          if (file_meta.being_compacted) {
            return Status::Aborted(
                "Specified compaction input file " + std::to_string(file_num) +
                " is already being compacted.");
          }
          found = true;
          break;
        }
      }
      if (found) {
        break;
      }
    }
    if (!found) {
      return Status::InvalidArgument(
          "Specified compaction input file " + std::to_string(file_num) +
          " does not exist in column family " + cf_meta.name + ".");
    }
  }

  return Status::OK();
}

Compaction* CompactionPicker::FormCompaction(
      const CompactionOptions& compact_options,
      const autovector<CompactionInputFiles>& input_files,
      int output_level, Version* version,
      const MutableCFOptions& mutable_cf_options) const {
  uint64_t max_grandparent_overlap_bytes = 0;
  if (output_level != kDeletionCompaction) {
      output_level + 1 < NumberLevels() ?
          mutable_cf_options.MaxGrandParentOverlapBytes(output_level + 1) :
          std::numeric_limits<uint64_t>::max();
  }
  assert(input_files.size());
  auto c = new Compaction(version, input_files,
      input_files[0].level, output_level,
      max_grandparent_overlap_bytes, compact_options,
      (output_level == kDeletionCompaction));
  c->MarkFilesBeingCompacted(true);
  c->SetupBottomMostLevel((output_level == NumberLevels() - 1));
  c->mutable_cf_options_ = mutable_cf_options;
  return c;
}

Compaction* LevelCompactionPicker::PickCompaction(
    const MutableCFOptions& mutable_cf_options,
    Version* version, LogBuffer* log_buffer) {
  Compaction* c = nullptr;
  int level = -1;

  // Compute the compactions needed. It is better to do it here
  // and also in LogAndApply(), otherwise the values could be stale.
  std::vector<uint64_t> size_being_compacted(NumberLevels() - 1);
  SizeBeingCompacted(size_being_compacted);
  version->ComputeCompactionScore(mutable_cf_options, size_being_compacted);

  // We prefer compactions triggered by too much data in a level over
  // the compactions triggered by seeks.
  //
  // Find the compactions by size on all levels.
  for (int i = 0; i < NumberLevels() - 1; i++) {
    assert(i == 0 ||
           version->compaction_score_[i] <= version->compaction_score_[i - 1]);
    level = version->compaction_level_[i];
    if ((version->compaction_score_[i] >= 1)) {
      c = PickCompactionBySize(mutable_cf_options, version, level,
          version->compaction_score_[i]);
      if (c == nullptr || ExpandWhileOverlapping(c) == false) {
        delete c;
        c = nullptr;
      } else {
        break;
      }
    }
  }

  if (c == nullptr) {
    return nullptr;
  }

  // Two level 0 compaction won't run at the same time, so don't need to worry
  // about files on level 0 being compacted.
  if (level == 0) {
    assert(compactions_in_progress_[0].empty());
    InternalKey smallest, largest;
    GetRange(c->inputs_[0].files, &smallest, &largest);
    // Note that the next call will discard the file we placed in
    // c->inputs_[0] earlier and replace it with an overlapping set
    // which will include the picked file.
    c->inputs_[0].clear();
    c->input_version_->GetOverlappingInputs(0, &smallest, &largest,
                                            &c->inputs_[0].files);

    // If we include more L0 files in the same compaction run it can
    // cause the 'smallest' and 'largest' key to get extended to a
    // larger range. So, re-invoke GetRange to get the new key range
    GetRange(c->inputs_[0].files, &smallest, &largest);
    if (ParentRangeInCompaction(c->input_version_, &smallest, &largest, level,
                                &c->parent_index_)) {
      delete c;
      return nullptr;
    }
    assert(!c->inputs_[0].empty());
  }

  // Setup "level+1" files (inputs_[1])
  SetupOtherInputs(mutable_cf_options, c);

  // mark all the files that are being compacted
  c->MarkFilesBeingCompacted(true);

  // Is this compaction creating a file at the bottommost level
  c->SetupBottomMostLevel(false);

  // remember this currently undergoing compaction
  compactions_in_progress_[level].insert(c);

  c->mutable_cf_options_ = mutable_cf_options;
  return c;
}

Compaction* LevelCompactionPicker::PickCompactionBySize(
    const MutableCFOptions& mutable_cf_options,
    Version* version, int level, double score) {
  Compaction* c = nullptr;

  // level 0 files are overlapping. So we cannot pick more
  // than one concurrent compactions at this level. This
  // could be made better by looking at key-ranges that are
  // being compacted at level 0.
  if (level == 0 && compactions_in_progress_[level].size() == 1) {
    return nullptr;
  }

  assert(level >= 0);
  assert(level + 1 < NumberLevels());
  c = new Compaction(version, level, level + 1,
                     mutable_cf_options.MaxFileSizeForLevel(level + 1),
                     mutable_cf_options.MaxGrandParentOverlapBytes(level), 0,
                     GetCompressionType(ioptions_, level + 1));
  c->score_ = score;

  // Pick the largest file in this level that is not already
  // being compacted
  std::vector<int>& file_size = c->input_version_->files_by_size_[level];

  // record the first file that is not yet compacted
  int nextIndex = -1;

  for (unsigned int i = c->input_version_->next_file_to_compact_by_size_[level];
       i < file_size.size(); i++) {
    int index = file_size[i];
    FileMetaData* f = c->input_version_->files_[level][index];

    // Check to verify files are arranged in descending compensated size.
    assert((i == file_size.size() - 1) ||
           (i >= Version::number_of_files_to_sort_ - 1) ||
           (f->compensated_file_size >=
            c->input_version_->files_[level][file_size[i + 1]]->
                compensated_file_size));

    // do not pick a file to compact if it is being compacted
    // from n-1 level.
    if (f->being_compacted) {
      continue;
    }

    // remember the startIndex for the next call to PickCompaction
    if (nextIndex == -1) {
      nextIndex = i;
    }

    // Do not pick this file if its parents at level+1 are being compacted.
    // Maybe we can avoid redoing this work in SetupOtherInputs
    int parent_index = -1;
    if (ParentRangeInCompaction(c->input_version_, &f->smallest, &f->largest,
                                level, &parent_index)) {
      continue;
    }
    c->inputs_[0].files.push_back(f);
    c->base_index_ = index;
    c->parent_index_ = parent_index;
    break;
  }

  if (c->inputs_[0].empty()) {
    delete c;
    c = nullptr;
  }

  // store where to start the iteration in the next call to PickCompaction
  version->next_file_to_compact_by_size_[level] = nextIndex;

  return c;
}

// Universal style of compaction. Pick files that are contiguous in
// time-range to compact.
//
Compaction* UniversalCompactionPicker::PickCompaction(
    const MutableCFOptions& mutable_cf_options,
    Version* version, LogBuffer* log_buffer) {
  int level = 0;
  double score = version->compaction_score_[0];

  if ((version->files_[level].size() <
       (unsigned int)mutable_cf_options.level0_file_num_compaction_trigger)) {
    LogToBuffer(log_buffer, "[%s] Universal: nothing to do\n",
                version->cfd_->GetName().c_str());
    return nullptr;
  }
  Version::FileSummaryStorage tmp;
  LogToBuffer(log_buffer, 3072, "[%s] Universal: candidate files(%zu): %s\n",
              version->cfd_->GetName().c_str(), version->files_[level].size(),
              version->LevelFileSummary(&tmp, 0));

  // Check for size amplification first.
  Compaction* c;
  if ((c = PickCompactionUniversalSizeAmp(
          mutable_cf_options, version, score, log_buffer)) != nullptr) {
    LogToBuffer(log_buffer, "[%s] Universal: compacting for size amp\n",
                version->cfd_->GetName().c_str());
  } else {
    // Size amplification is within limits. Try reducing read
    // amplification while maintaining file size ratios.
    unsigned int ratio = ioptions_.compaction_options_universal.size_ratio;

    if ((c = PickCompactionUniversalReadAmp(
            mutable_cf_options, version, score, ratio,
            UINT_MAX, log_buffer)) != nullptr) {
      LogToBuffer(log_buffer, "[%s] Universal: compacting for size ratio\n",
                  version->cfd_->GetName().c_str());
    } else {
      // Size amplification and file size ratios are within configured limits.
      // If max read amplification is exceeding configured limits, then force
      // compaction without looking at filesize ratios and try to reduce
      // the number of files to fewer than level0_file_num_compaction_trigger.
      unsigned int num_files = version->files_[level].size() -
          mutable_cf_options.level0_file_num_compaction_trigger;
      if ((c = PickCompactionUniversalReadAmp(
               mutable_cf_options, version, score, UINT_MAX,
               num_files, log_buffer)) != nullptr) {
        LogToBuffer(log_buffer, "[%s] Universal: compacting for file num\n",
                    version->cfd_->GetName().c_str());
      }
    }
  }
  if (c == nullptr) {
    return nullptr;
  }
  assert(c->inputs_[0].size() > 1);

  // validate that all the chosen files are non overlapping in time
  FileMetaData* newerfile __attribute__((unused)) = nullptr;
  for (unsigned int i = 0; i < c->inputs_[0].size(); i++) {
    FileMetaData* f = c->inputs_[0][i];
    assert (f->smallest_seqno <= f->largest_seqno);
    assert(newerfile == nullptr ||
           newerfile->smallest_seqno > f->largest_seqno);
    newerfile = f;
  }

  // Is the earliest file part of this compaction?
  FileMetaData* last_file = c->input_version_->files_[level].back();
  c->bottommost_level_ = c->inputs_[0].files.back() == last_file;

  // update statistics
  MeasureTime(ioptions_.statistics,
              NUM_FILES_IN_SINGLE_COMPACTION, c->inputs_[0].size());

  // mark all the files that are being compacted
  c->MarkFilesBeingCompacted(true);

  // remember this currently undergoing compaction
  compactions_in_progress_[level].insert(c);

  // Record whether this compaction includes all sst files.
  // For now, it is only relevant in universal compaction mode.
  c->is_full_compaction_ =
      (c->inputs_[0].size() == c->input_version_->files_[0].size());

  c->mutable_cf_options_ = mutable_cf_options;
  return c;
}

uint32_t UniversalCompactionPicker::GetPathId(
    const ImmutableCFOptions& ioptions, uint64_t file_size) {
  // Two conditions need to be satisfied:
  // (1) the target path needs to be able to hold the file's size
  // (2) Total size left in this and previous paths need to be not
  //     smaller than expected future file size before this new file is
  //     compacted, which is estimated based on size_ratio.
  // For example, if now we are compacting files of size (1, 1, 2, 4, 8),
  // we will make sure the target file, probably with size of 16, will be
  // placed in a path so that eventually when new files are generated and
  // compacted to (1, 1, 2, 4, 8, 16), all those files can be stored in or
  // before the path we chose.
  //
  // TODO(sdong): now the case of multiple column families is not
  // considered in this algorithm. So the target size can be violated in
  // that case. We need to improve it.
  uint64_t accumulated_size = 0;
  uint64_t future_size = file_size *
    (100 - ioptions.compaction_options_universal.size_ratio) / 100;
  uint32_t p = 0;
  for (; p < ioptions.db_paths.size() - 1; p++) {
    uint64_t target_size = ioptions.db_paths[p].target_size;
    if (target_size > file_size &&
        accumulated_size + (target_size - file_size) > future_size) {
      return p;
    }
    accumulated_size += target_size;
  }
  return p;
}

//
// Consider compaction files based on their size differences with
// the next file in time order.
//
Compaction* UniversalCompactionPicker::PickCompactionUniversalReadAmp(
    const MutableCFOptions& mutable_cf_options, Version* version,
    double score, unsigned int ratio,
    unsigned int max_number_of_files_to_compact, LogBuffer* log_buffer) {
  int level = 0;

  unsigned int min_merge_width =
    ioptions_.compaction_options_universal.min_merge_width;
  unsigned int max_merge_width =
    ioptions_.compaction_options_universal.max_merge_width;

  // The files are sorted from newest first to oldest last.
  const auto& files = version->files_[level];

  FileMetaData* f = nullptr;
  bool done = false;
  int start_index = 0;
  unsigned int candidate_count = 0;

  unsigned int max_files_to_compact = std::min(max_merge_width,
                                       max_number_of_files_to_compact);
  min_merge_width = std::max(min_merge_width, 2U);

  // Considers a candidate file only if it is smaller than the
  // total size accumulated so far.
  for (unsigned int loop = 0; loop < files.size(); loop++) {

    candidate_count = 0;

    // Skip files that are already being compacted
    for (f = nullptr; loop < files.size(); loop++) {
      f = files[loop];

      if (!f->being_compacted) {
        candidate_count = 1;
        break;
      }
      LogToBuffer(log_buffer, "[%s] Universal: file %" PRIu64
                              "[%d] being compacted, skipping",
                  version->cfd_->GetName().c_str(), f->fd.GetNumber(), loop);
      f = nullptr;
    }

    // This file is not being compacted. Consider it as the
    // first candidate to be compacted.
    uint64_t candidate_size =  f != nullptr? f->compensated_file_size : 0;
    if (f != nullptr) {
      char file_num_buf[kFormatFileNumberBufSize];
      FormatFileNumber(f->fd.GetNumber(), f->fd.GetPathId(), file_num_buf,
                       sizeof(file_num_buf));
      LogToBuffer(log_buffer, "[%s] Universal: Possible candidate file %s[%d].",
                  version->cfd_->GetName().c_str(), file_num_buf, loop);
    }

    // Check if the suceeding files need compaction.
    for (unsigned int i = loop + 1;
         candidate_count < max_files_to_compact && i < files.size(); i++) {
      FileMetaData* f = files[i];
      if (f->being_compacted) {
        break;
      }
      // Pick files if the total/last candidate file size (increased by the
      // specified ratio) is still larger than the next candidate file.
      // candidate_size is the total size of files picked so far with the
      // default kCompactionStopStyleTotalSize; with
      // kCompactionStopStyleSimilarSize, it's simply the size of the last
      // picked file.
      double sz = candidate_size * (100.0 + ratio) / 100.0;
      if (sz < static_cast<double>(f->fd.GetFileSize())) {
        break;
      }
      if (ioptions_.compaction_options_universal.stop_style ==
          kCompactionStopStyleSimilarSize) {
        // Similar-size stopping rule: also check the last picked file isn't
        // far larger than the next candidate file.
        sz = (f->fd.GetFileSize() * (100.0 + ratio)) / 100.0;
        if (sz < static_cast<double>(candidate_size)) {
          // If the small file we've encountered begins a run of similar-size
          // files, we'll pick them up on a future iteration of the outer
          // loop. If it's some lonely straggler, it'll eventually get picked
          // by the last-resort read amp strategy which disregards size ratios.
          break;
        }
        candidate_size = f->compensated_file_size;
      } else { // default kCompactionStopStyleTotalSize
        candidate_size += f->compensated_file_size;
      }
      candidate_count++;
    }

    // Found a series of consecutive files that need compaction.
    if (candidate_count >= (unsigned int)min_merge_width) {
      start_index = loop;
      done = true;
      break;
    } else {
      for (unsigned int i = loop;
           i < loop + candidate_count && i < files.size(); i++) {
        FileMetaData* f = files[i];
        LogToBuffer(log_buffer, "[%s] Universal: Skipping file %" PRIu64
                                "[%d] with size %" PRIu64
                                " (compensated size %" PRIu64 ") %d\n",
                    version->cfd_->GetName().c_str(), f->fd.GetNumber(), i,
                    f->fd.GetFileSize(), f->compensated_file_size,
                    f->being_compacted);
      }
    }
  }
  if (!done || candidate_count <= 1) {
    return nullptr;
  }
  unsigned int first_index_after = start_index + candidate_count;
  // Compression is enabled if files compacted earlier already reached
  // size ratio of compression.
  bool enable_compression = true;
  int ratio_to_compress =
      ioptions_.compaction_options_universal.compression_size_percent;
  if (ratio_to_compress >= 0) {
    uint64_t total_size = version->NumLevelBytes(level);
    uint64_t older_file_size = 0;
    for (unsigned int i = files.size() - 1;
         i >= first_index_after; i--) {
      older_file_size += files[i]->fd.GetFileSize();
      if (older_file_size * 100L >= total_size * (long) ratio_to_compress) {
        enable_compression = false;
        break;
      }
    }
  }

  uint64_t estimated_total_size = 0;
  for (unsigned int i = 0; i < first_index_after; i++) {
    estimated_total_size += files[i]->fd.GetFileSize();
  }
  uint32_t path_id = GetPathId(ioptions_, estimated_total_size);

  Compaction* c = new Compaction(
      version, level, level, mutable_cf_options.MaxFileSizeForLevel(level),
      LLONG_MAX, path_id, GetCompressionType(ioptions_, level,
      enable_compression));
  c->score_ = score;

  for (unsigned int i = start_index; i < first_index_after; i++) {
    FileMetaData* f = c->input_version_->files_[level][i];
    c->inputs_[0].files.push_back(f);
    char file_num_buf[kFormatFileNumberBufSize];
    FormatFileNumber(f->fd.GetNumber(), f->fd.GetPathId(), file_num_buf,
                     sizeof(file_num_buf));
    LogToBuffer(log_buffer,
                "[%s] Universal: Picking file %s[%d] "
                "with size %" PRIu64 " (compensated size %" PRIu64 ")\n",
                version->cfd_->GetName().c_str(), file_num_buf, i,
                f->fd.GetFileSize(), f->compensated_file_size);
  }
  return c;
}

// Look at overall size amplification. If size amplification
// exceeeds the configured value, then do a compaction
// of the candidate files all the way upto the earliest
// base file (overrides configured values of file-size ratios,
// min_merge_width and max_merge_width).
//
Compaction* UniversalCompactionPicker::PickCompactionUniversalSizeAmp(
    const MutableCFOptions& mutable_cf_options, Version* version,
    double score, LogBuffer* log_buffer) {
  int level = 0;

  // percentage flexibilty while reducing size amplification
  uint64_t ratio = ioptions_.compaction_options_universal.
                     max_size_amplification_percent;

  // The files are sorted from newest first to oldest last.
  const auto& files = version->files_[level];

  unsigned int candidate_count = 0;
  uint64_t candidate_size = 0;
  unsigned int start_index = 0;
  FileMetaData* f = nullptr;

  // Skip files that are already being compacted
  for (unsigned int loop = 0; loop < files.size() - 1; loop++) {
    f = files[loop];
    if (!f->being_compacted) {
      start_index = loop;         // Consider this as the first candidate.
      break;
    }
    char file_num_buf[kFormatFileNumberBufSize];
    FormatFileNumber(f->fd.GetNumber(), f->fd.GetPathId(), file_num_buf,
                     sizeof(file_num_buf));
    LogToBuffer(log_buffer, "[%s] Universal: skipping file %s[%d] compacted %s",
                version->cfd_->GetName().c_str(), file_num_buf, loop,
                " cannot be a candidate to reduce size amp.\n");
    f = nullptr;
  }
  if (f == nullptr) {
    return nullptr;             // no candidate files
  }

  char file_num_buf[kFormatFileNumberBufSize];
  FormatFileNumber(f->fd.GetNumber(), f->fd.GetPathId(), file_num_buf,
                   sizeof(file_num_buf));
  LogToBuffer(log_buffer, "[%s] Universal: First candidate file %s[%d] %s",
              version->cfd_->GetName().c_str(), file_num_buf, start_index,
              " to reduce size amp.\n");

  // keep adding up all the remaining files
  for (unsigned int loop = start_index; loop < files.size() - 1; loop++) {
    f = files[loop];
    if (f->being_compacted) {
      char file_num_buf[kFormatFileNumberBufSize];
      FormatFileNumber(f->fd.GetNumber(), f->fd.GetPathId(), file_num_buf,
                       sizeof(file_num_buf));
      LogToBuffer(
          log_buffer, "[%s] Universal: Possible candidate file %s[%d] %s.",
          version->cfd_->GetName().c_str(), file_num_buf, loop,
          " is already being compacted. No size amp reduction possible.\n");
      return nullptr;
    }
    candidate_size += f->compensated_file_size;
    candidate_count++;
  }
  if (candidate_count == 0) {
    return nullptr;
  }

  // size of earliest file
  uint64_t earliest_file_size = files.back()->fd.GetFileSize();

  // size amplification = percentage of additional size
  if (candidate_size * 100 < ratio * earliest_file_size) {
    LogToBuffer(
        log_buffer,
        "[%s] Universal: size amp not needed. newer-files-total-size %" PRIu64
        "earliest-file-size %" PRIu64,
        version->cfd_->GetName().c_str(), candidate_size, earliest_file_size);
    return nullptr;
  } else {
    LogToBuffer(
        log_buffer,
        "[%s] Universal: size amp needed. newer-files-total-size %" PRIu64
        "earliest-file-size %" PRIu64,
        version->cfd_->GetName().c_str(), candidate_size, earliest_file_size);
  }
  assert(start_index < files.size() - 1);

  // Estimate total file size
  uint64_t estimated_total_size = 0;
  for (unsigned int loop = start_index; loop < files.size(); loop++) {
    estimated_total_size += files[loop]->fd.GetFileSize();
  }
  uint32_t path_id = GetPathId(ioptions_, estimated_total_size);

  // create a compaction request
  // We always compact all the files, so always compress.
  Compaction* c =
      new Compaction(version, level, level,
                     mutable_cf_options.MaxFileSizeForLevel(level),
                     LLONG_MAX, path_id, GetCompressionType(ioptions_, level));
  c->score_ = score;
  for (unsigned int loop = start_index; loop < files.size(); loop++) {
    f = c->input_version_->files_[level][loop];
    c->inputs_[0].files.push_back(f);
    LogToBuffer(log_buffer,
        "[%s] Universal: size amp picking file %" PRIu64 "[%d] "
        "with size %" PRIu64 " (compensated size %" PRIu64 ")",
        version->cfd_->GetName().c_str(),
        f->fd.GetNumber(), loop,
        f->fd.GetFileSize(), f->compensated_file_size);
  }
  return c;
}

Compaction* FIFOCompactionPicker::PickCompaction(
    const MutableCFOptions& mutable_cf_options,
    Version* version, LogBuffer* log_buffer) {
  assert(version->NumberLevels() == 1);
  uint64_t total_size = 0;
  for (const auto& file : version->files_[0]) {
    total_size += file->compensated_file_size;
  }

  if (total_size <= ioptions_.compaction_options_fifo.max_table_files_size ||
      version->files_[0].size() == 0) {
    // total size not exceeded
    LogToBuffer(log_buffer,
                "[%s] FIFO compaction: nothing to do. Total size %" PRIu64
                ", max size %" PRIu64 "\n",
                version->cfd_->GetName().c_str(), total_size,
                ioptions_.compaction_options_fifo.max_table_files_size);
    return nullptr;
  }

  if (compactions_in_progress_[0].size() > 0) {
    LogToBuffer(log_buffer,
                "[%s] FIFO compaction: Already executing compaction. No need "
                "to run parallel compactions since compactions are very fast",
                version->cfd_->GetName().c_str());
    return nullptr;
  }

  Compaction* c = new Compaction(version, 0, 0, 0, 0, 0, kNoCompression, false,
                                 true /* is deletion compaction */);
  // delete old files (FIFO)
  for (auto ritr = version->files_[0].rbegin();
       ritr != version->files_[0].rend(); ++ritr) {
    auto f = *ritr;
    total_size -= f->compensated_file_size;
    c->inputs_[0].files.push_back(f);
    char tmp_fsize[16];
    AppendHumanBytes(f->fd.GetFileSize(), tmp_fsize, sizeof(tmp_fsize));
    LogToBuffer(log_buffer, "[%s] FIFO compaction: picking file %" PRIu64
                            " with size %s for deletion",
                version->cfd_->GetName().c_str(), f->fd.GetNumber(), tmp_fsize);
    if (total_size <= ioptions_.compaction_options_fifo.max_table_files_size) {
      break;
    }
  }

  c->MarkFilesBeingCompacted(true);
  compactions_in_progress_[0].insert(c);
  c->mutable_cf_options_ = mutable_cf_options;
  return c;
}

Compaction* FIFOCompactionPicker::CompactRange(
    const MutableCFOptions& mutable_cf_options,
    Version* version, int input_level, int output_level,
    uint32_t output_path_id, const InternalKey* begin, const InternalKey* end,
    InternalKey** compaction_end) {
  assert(input_level == 0);
  assert(output_level == 0);
  *compaction_end = nullptr;
  LogBuffer log_buffer(InfoLogLevel::INFO_LEVEL, ioptions_.info_log);
  Compaction* c = PickCompaction(mutable_cf_options, version, &log_buffer);
  if (c != nullptr) {
    assert(output_path_id < static_cast<uint32_t>(ioptions_.db_paths.size()));
    c->output_path_id_ = output_path_id;
  }
  log_buffer.FlushBufferToLog();
  return c;
}

Compaction* PluggableCompactionPicker::PickCompaction(
      const MutableCFOptions& mutable_cf_options,
      Version* version, LogBuffer* log_buffer) {
  assert(version);
  if (compactor_ == nullptr) {
    return nullptr;
  }

  ColumnFamilyMetaData cf_meta;
  std::vector<uint64_t> input_file_numbers;
  int output_level;
  version->GetColumnFamilyMetaData(&cf_meta, ioptions_);
  Status s = compactor_->PickCompaction(
      &input_file_numbers, &output_level, cf_meta);
  if (!s.ok() || input_file_numbers.size() == 0U) {
    return nullptr;
  }

  std::set<uint64_t> input_set(
      input_file_numbers.begin(), input_file_numbers.end());

  s = SanitizeCompactionInputFiles(
      &input_set, cf_meta, output_level);
  if (!s.ok()) {
    return nullptr;
  }

  autovector<CompactionInputFiles> input_files;
  s = GetCompactionInputsFromFileNumbers(
      &input_files, &input_set, version,
      compactor_->compact_options());
  if (!s.ok() || input_files.size() == 0U) {
    return nullptr;
  }

  return FormCompaction(
      compactor_->compact_options(), input_files,
      output_level, version, mutable_cf_options);
}

Compaction* PluggableCompactionPicker::CompactRange(
    const MutableCFOptions& mutable_cf_options,
    Version* version, int input_level,
    int output_level, uint32_t output_path_id,
    const InternalKey* begin,
    const InternalKey* end,
    InternalKey** compaction_end) {
  assert(version);
  if (compactor_ == nullptr) {
    return nullptr;
  }

  ColumnFamilyMetaData cf_meta;
  std::vector<uint64_t> input_file_numbers;
  version->GetColumnFamilyMetaData(&cf_meta, ioptions_);
  Status s = compactor_->PickCompactionByRange(
      &input_file_numbers, cf_meta, input_level, output_level);
  if (!s.ok()) {
    return nullptr;
  }

  std::set<uint64_t> input_set(
      input_file_numbers.begin(), input_file_numbers.end());
  s = SanitizeCompactionInputFiles(
      &input_set, cf_meta, output_level);
  if (!s.ok()) {
    return nullptr;
  }

  // TODO(yhchiang): may need to add CompactionOptions to
  // CompactRange().
  CompactionOptions compact_options;
  autovector<CompactionInputFiles> input_files;
  s = GetCompactionInputsFromFileNumbers(
      &input_files, &input_set, version, compact_options);
  if (!s.ok() || input_files.size() == 0U) {
    return nullptr;
  }

  return FormCompaction(
      compact_options, input_files,
      output_level, version, mutable_cf_options);
}


}  // namespace rocksdb
