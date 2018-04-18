/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <inttypes.h>

#include <memory>

#include <android-base/strings.h>

#include "system/extras/simpleperf/report_sample.pb.h"

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

#include "command.h"
#include "event_attr.h"
#include "event_type.h"
#include "record_file.h"
#include "thread_tree.h"
#include "utils.h"

namespace proto = simpleperf_report_proto;

namespace {

static const char PROT_FILE_MAGIC[] = "SIMPLEPERF";
static const uint16_t PROT_FILE_VERSION = 1u;

class ProtobufFileWriter : public google::protobuf::io::CopyingOutputStream {
 public:
  explicit ProtobufFileWriter(FILE* out_fp) : out_fp_(out_fp) {}

  bool Write(const void* buffer, int size) override {
    return fwrite(buffer, size, 1, out_fp_) == 1;
  }

 private:
  FILE* out_fp_;
};

class ProtobufFileReader : public google::protobuf::io::CopyingInputStream {
 public:
  explicit ProtobufFileReader(FILE* in_fp) : in_fp_(in_fp) {}

  int Read(void* buffer, int size) override {
    return fread(buffer, 1, size, in_fp_);
  }

 private:
  FILE* in_fp_;
};

struct CallEntry {
  Dso* dso;
  const Symbol* symbol;
  uint64_t vaddr_in_file;
};

class ReportSampleCommand : public Command {
 public:
  ReportSampleCommand()
      : Command(
            "report-sample", "report raw sample information in perf.data",
            // clang-format off
"Usage: simpleperf report-sample [options]\n"
"--dump-protobuf-report  <file>\n"
"           Dump report file generated by\n"
"           `simpleperf report-sample --protobuf -o <file>`.\n"
"-i <file>  Specify path of record file, default is perf.data.\n"
"-o report_file_name  Set report file name. Default report file name is\n"
"                     report_sample.trace if --protobuf is used, otherwise\n"
"                     the report is written to stdout.\n"
"--protobuf  Use protobuf format in report_sample.proto to output samples.\n"
"            Need to set a report_file_name when using this option.\n"
"--show-callchain  Print callchain samples.\n"
"--remove-unknown-kernel-symbols  Remove kernel callchains when kernel symbols\n"
"                                 are not available in perf.data.\n"
"--show-art-frames  Show frames of internal methods in the ART Java interpreter.\n"
            // clang-format on
            ),
        record_filename_("perf.data"),
        show_callchain_(false),
        use_protobuf_(false),
        report_fp_(nullptr),
        coded_os_(nullptr),
        sample_count_(0),
        lost_count_(0),
        trace_offcpu_(false),
        remove_unknown_kernel_symbols_(false),
        kernel_symbols_available_(false),
        show_art_frames_(false) {}

  bool Run(const std::vector<std::string>& args) override;

 private:
  bool ParseOptions(const std::vector<std::string>& args);
  bool DumpProtobufReport(const std::string& filename);
  bool OpenRecordFile();
  bool PrintMetaInfo();
  bool ProcessRecord(std::unique_ptr<Record> record);
  bool ProcessSampleRecord(const SampleRecord& r);
  bool PrintSampleRecordInProtobuf(const SampleRecord& record,
                                   const std::vector<CallEntry>& entries);
  bool GetCallEntry(const ThreadEntry* thread, bool in_kernel, uint64_t ip, bool omit_unknown_dso,
                    CallEntry* entry);
  bool WriteRecordInProtobuf(proto::Record& proto_record);
  bool PrintLostSituationInProtobuf();
  bool PrintFileInfoInProtobuf();
  bool PrintThreadInfoInProtobuf();
  bool PrintSampleRecord(const SampleRecord& record, const std::vector<CallEntry>& entries);
  void PrintLostSituation();

  std::string record_filename_;
  std::unique_ptr<RecordFileReader> record_file_reader_;
  std::string dump_protobuf_report_file_;
  bool show_callchain_;
  bool use_protobuf_;
  ThreadTree thread_tree_;
  std::string report_filename_;
  FILE* report_fp_;
  google::protobuf::io::CodedOutputStream* coded_os_;
  size_t sample_count_;
  size_t lost_count_;
  bool trace_offcpu_;
  std::unique_ptr<ScopedEventTypes> scoped_event_types_;
  std::vector<std::string> event_types_;
  std::unordered_map<std::string, std::string> meta_info_;
  bool remove_unknown_kernel_symbols_;
  bool kernel_symbols_available_;
  bool show_art_frames_;
};

bool ReportSampleCommand::Run(const std::vector<std::string>& args) {
  // 1. Parse options.
  if (!ParseOptions(args)) {
    return false;
  }
  // 2. Prepare report fp.
  report_fp_ = stdout;
  std::unique_ptr<FILE, decltype(&fclose)> fp(nullptr, fclose);
  if (!report_filename_.empty()) {
    const char* open_mode = "w";
    if (!dump_protobuf_report_file_.empty() && use_protobuf_) {
      open_mode = "wb";
    }
    fp.reset(fopen(report_filename_.c_str(), open_mode));
    if (fp == nullptr) {
      PLOG(ERROR) << "failed to open " << report_filename_;
      return false;
    }
    report_fp_ = fp.get();
  }

  // 3. Dump protobuf report.
  if (!dump_protobuf_report_file_.empty()) {
    return DumpProtobufReport(dump_protobuf_report_file_);
  }

  // 4. Open record file.
  if (!OpenRecordFile()) {
    return false;
  }
  if (use_protobuf_) {
    GOOGLE_PROTOBUF_VERIFY_VERSION;
  } else {
    thread_tree_.ShowMarkForUnknownSymbol();
    thread_tree_.ShowIpForUnknownSymbol();
  }

  // 5. Prepare protobuf output stream.
  std::unique_ptr<ProtobufFileWriter> protobuf_writer;
  std::unique_ptr<google::protobuf::io::CopyingOutputStreamAdaptor> protobuf_os;
  std::unique_ptr<google::protobuf::io::CodedOutputStream> protobuf_coded_os;
  if (use_protobuf_) {
    if (fprintf(report_fp_, "%s", PROT_FILE_MAGIC) != 10 ||
        fwrite(&PROT_FILE_VERSION, sizeof(uint16_t), 1, report_fp_) != 1u) {
      PLOG(ERROR) << "Failed to write magic/version";
      return false;
    }
    protobuf_writer.reset(new ProtobufFileWriter(report_fp_));
    protobuf_os.reset(new google::protobuf::io::CopyingOutputStreamAdaptor(
        protobuf_writer.get()));
    protobuf_coded_os.reset(
        new google::protobuf::io::CodedOutputStream(protobuf_os.get()));
    coded_os_ = protobuf_coded_os.get();
  }

  // 6. Read record file, and print samples online.
  if (!PrintMetaInfo()) {
    return false;
  }
  if (!record_file_reader_->ReadDataSection(
          [this](std::unique_ptr<Record> record) {
            return ProcessRecord(std::move(record));
          })) {
    return false;
  }

  if (use_protobuf_) {
    if (!PrintLostSituationInProtobuf()) {
      return false;
    }
    if (!PrintFileInfoInProtobuf()) {
      return false;
    }
    if (!PrintThreadInfoInProtobuf()) {
      return false;
    }
    coded_os_->WriteLittleEndian32(0);
    if (coded_os_->HadError()) {
      LOG(ERROR) << "print protobuf report failed";
      return false;
    }
    protobuf_coded_os.reset(nullptr);
  } else {
    PrintLostSituation();
    fflush(report_fp_);
  }
  if (ferror(report_fp_) != 0) {
    PLOG(ERROR) << "print report failed";
    return false;
  }
  return true;
}

bool ReportSampleCommand::ParseOptions(const std::vector<std::string>& args) {
  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--dump-protobuf-report") {
      if (!NextArgumentOrError(args, &i)) {
        return false;
      }
      dump_protobuf_report_file_ = args[i];
    } else if (args[i] == "-i") {
      if (!NextArgumentOrError(args, &i)) {
        return false;
      }
      record_filename_ = args[i];
    } else if (args[i] == "-o") {
      if (!NextArgumentOrError(args, &i)) {
        return false;
      }
      report_filename_ = args[i];
    } else if (args[i] == "--protobuf") {
      use_protobuf_ = true;
    } else if (args[i] == "--show-callchain") {
      show_callchain_ = true;
    } else if (args[i] == "--remove-unknown-kernel-symbols") {
      remove_unknown_kernel_symbols_ = true;
    } else if (args[i] == "--show-art-frames") {
      show_art_frames_ = true;
    } else {
      ReportUnknownOption(args, i);
      return false;
    }
  }

  if (use_protobuf_ && report_filename_.empty()) {
    report_filename_ = "report_sample.trace";
  }
  return true;
}

bool ReportSampleCommand::DumpProtobufReport(const std::string& filename) {
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  std::unique_ptr<FILE, decltype(&fclose)> fp(fopen(filename.c_str(), "rb"),
                                              fclose);
  if (fp == nullptr) {
    PLOG(ERROR) << "failed to open " << filename;
    return false;
  }
  char magic[11] = {};
  if (fread(magic, 10, 1, fp.get()) != 1u || memcmp(magic, PROT_FILE_MAGIC, 10) != 0) {
    PLOG(ERROR) << filename << " isn't a file generated by report-sample command.";
    return false;
  }
  FprintIndented(report_fp_, 0, "magic: %s\n", magic);
  uint16_t version;
  if (fread(&version, sizeof(uint16_t), 1, fp.get()) != 1u || version != PROT_FILE_VERSION) {
    PLOG(ERROR) << filename << " doesn't have the expected version.";
    return false;
  }
  FprintIndented(report_fp_, 0, "version: %u\n", version);

  ProtobufFileReader protobuf_reader(fp.get());
  google::protobuf::io::CopyingInputStreamAdaptor adaptor(&protobuf_reader);
  google::protobuf::io::CodedInputStream coded_is(&adaptor);
  // map from file_id to max_symbol_id requested on the file.
  std::unordered_map<uint32_t, int32_t> max_symbol_id_map;
  // files[file_id] is the number of symbols in the file.
  std::vector<uint32_t> files;
  uint32_t max_message_size = 64 * (1 << 20);
  uint32_t warning_message_size = 512 * (1 << 20);
  coded_is.SetTotalBytesLimit(max_message_size, warning_message_size);
  while (true) {
    uint32_t size;
    if (!coded_is.ReadLittleEndian32(&size)) {
      PLOG(ERROR) << "failed to read " << filename;
      return false;
    }
    if (size == 0) {
      break;
    }
    // Handle files having large symbol table.
    if (size > max_message_size) {
      max_message_size = size;
      coded_is.SetTotalBytesLimit(max_message_size, warning_message_size);
    }
    auto limit = coded_is.PushLimit(size);
    proto::Record proto_record;
    if (!proto_record.ParseFromCodedStream(&coded_is)) {
      PLOG(ERROR) << "failed to read " << filename;
      return false;
    }
    coded_is.PopLimit(limit);
    if (proto_record.has_sample()) {
      auto& sample = proto_record.sample();
      static size_t sample_count = 0;
      FprintIndented(report_fp_, 0, "sample %zu:\n", ++sample_count);
      FprintIndented(report_fp_, 1, "event_type_id: %zu\n", sample.event_type_id());
      FprintIndented(report_fp_, 1, "time: %" PRIu64 "\n", sample.time());
      FprintIndented(report_fp_, 1, "event_count: %" PRIu64 "\n", sample.event_count());
      FprintIndented(report_fp_, 1, "thread_id: %d\n", sample.thread_id());
      FprintIndented(report_fp_, 1, "callchain:\n");
      for (int i = 0; i < sample.callchain_size(); ++i) {
        const proto::Sample_CallChainEntry& callchain = sample.callchain(i);
        FprintIndented(report_fp_, 2, "vaddr_in_file: %" PRIx64 "\n",
                       callchain.vaddr_in_file());
        FprintIndented(report_fp_, 2, "file_id: %u\n", callchain.file_id());
        int32_t symbol_id = callchain.symbol_id();
        FprintIndented(report_fp_, 2, "symbol_id: %d\n", symbol_id);
        if (symbol_id < -1) {
          LOG(ERROR) << "unexpected symbol_id " << symbol_id;
          return false;
        }
        if (symbol_id != -1) {
          max_symbol_id_map[callchain.file_id()] =
              std::max(max_symbol_id_map[callchain.file_id()], symbol_id);
        }
      }
    } else if (proto_record.has_lost()) {
      auto& lost = proto_record.lost();
      FprintIndented(report_fp_, 0, "lost_situation:\n");
      FprintIndented(report_fp_, 1, "sample_count: %" PRIu64 "\n",
                     lost.sample_count());
      FprintIndented(report_fp_, 1, "lost_count: %" PRIu64 "\n",
                     lost.lost_count());
    } else if (proto_record.has_file()) {
      auto& file = proto_record.file();
      FprintIndented(report_fp_, 0, "file:\n");
      FprintIndented(report_fp_, 1, "id: %u\n", file.id());
      FprintIndented(report_fp_, 1, "path: %s\n", file.path().c_str());
      for (int i = 0; i < file.symbol_size(); ++i) {
        FprintIndented(report_fp_, 1, "symbol: %s\n", file.symbol(i).c_str());
      }
      for (int i = 0; i < file.mangled_symbol_size(); ++i) {
        FprintIndented(report_fp_, 1, "mangled_symbol: %s\n", file.mangled_symbol(i).c_str());
      }
      if (file.id() != files.size()) {
        LOG(ERROR) << "file id doesn't increase orderly, expected "
                   << files.size() << ", really " << file.id();
        return false;
      }
      files.push_back(file.symbol_size());
    } else if (proto_record.has_thread()) {
      auto& thread = proto_record.thread();
      FprintIndented(report_fp_, 0, "thread:\n");
      FprintIndented(report_fp_, 1, "thread_id: %u\n", thread.thread_id());
      FprintIndented(report_fp_, 1, "process_id: %u\n", thread.process_id());
      FprintIndented(report_fp_, 1, "thread_name: %s\n", thread.thread_name().c_str());
    } else if (proto_record.has_meta_info()) {
      auto& meta_info = proto_record.meta_info();
      FprintIndented(report_fp_, 0, "meta_info:\n");
      for (int i = 0; i < meta_info.event_type_size(); ++i) {
        FprintIndented(report_fp_, 1, "event_type: %s\n", meta_info.event_type(i).c_str());
      }
      if (meta_info.has_app_package_name()) {
        FprintIndented(report_fp_, 0, "app_package_name: %s\n",
                       meta_info.app_package_name().c_str());
      }
    } else {
      LOG(ERROR) << "unexpected record type ";
      return false;
    }
  }
  for (auto pair : max_symbol_id_map) {
    if (pair.first >= files.size()) {
      LOG(ERROR) << "file_id(" << pair.first << ") >= file count ("
                 << files.size() << ")";
      return false;
    }
    if (static_cast<uint32_t>(pair.second) >= files[pair.first]) {
      LOG(ERROR) << "symbol_id(" << pair.second << ") >= symbol count ("
                 << files[pair.first] << ") in file_id( " << pair.first << ")";
      return false;
    }
  }
  return true;
}

bool ReportSampleCommand::OpenRecordFile() {
  record_file_reader_ = RecordFileReader::CreateInstance(record_filename_);
  if (record_file_reader_ == nullptr) {
    return false;
  }
  record_file_reader_->LoadBuildIdAndFileFeatures(thread_tree_);
  if (record_file_reader_->HasFeature(PerfFileFormat::FEAT_META_INFO)) {
    if (!record_file_reader_->ReadMetaInfoFeature(&meta_info_)) {
      return false;
    }
    auto it = meta_info_.find("event_type_info");
    if (it != meta_info_.end()) {
      scoped_event_types_.reset(new ScopedEventTypes(it->second));
    }
    it = meta_info_.find("trace_offcpu");
    if (it != meta_info_.end()) {
      trace_offcpu_ = it->second == "true";
    }
    it = meta_info_.find("kernel_symbols_available");
    if (it != meta_info_.end()) {
      kernel_symbols_available_ = it->second == "true";
    }
  }
  for (EventAttrWithId& attr : record_file_reader_->AttrSection()) {
    event_types_.push_back(GetEventNameByAttr(*attr.attr));
  }
  return true;
}

bool ReportSampleCommand::PrintMetaInfo() {
  auto it = meta_info_.find("app_package_name");
  std::string app_package_name = it != meta_info_.end() ? it->second : "";
  if (use_protobuf_) {
    proto::Record proto_record;
    proto::MetaInfo* meta_info = proto_record.mutable_meta_info();
    for (auto& event_type : event_types_) {
      *(meta_info->add_event_type()) = event_type;
    }
    if (!app_package_name.empty()) {
      meta_info->set_app_package_name(app_package_name);
    }
    return WriteRecordInProtobuf(proto_record);
  }
  FprintIndented(report_fp_, 0, "meta_info:\n");
  FprintIndented(report_fp_, 1, "trace_offcpu: %s\n", trace_offcpu_ ? "true" : "false");
  for (auto& event_type : event_types_) {
    FprintIndented(report_fp_, 1, "event_type: %s\n", event_type.c_str());
  }
  if (!app_package_name.empty()) {
    FprintIndented(report_fp_, 1, "app_package_name: %s\n", app_package_name.c_str());
  }
  return true;
}

bool ReportSampleCommand::ProcessRecord(std::unique_ptr<Record> record) {
  thread_tree_.Update(*record);
  if (record->type() == PERF_RECORD_SAMPLE) {
    return ProcessSampleRecord(*static_cast<SampleRecord*>(record.get()));
  }
  if (record->type() == PERF_RECORD_LOST) {
    lost_count_ += static_cast<const LostRecord*>(record.get())->lost;
  }
  return true;
}

bool ReportSampleCommand::ProcessSampleRecord(const SampleRecord& r) {
  size_t kernel_ip_count;
  std::vector<uint64_t> ips = r.GetCallChain(&kernel_ip_count);
  if (kernel_ip_count > 0u && remove_unknown_kernel_symbols_ && !kernel_symbols_available_) {
    ips.erase(ips.begin(), ips.begin() + kernel_ip_count);
    kernel_ip_count = 0;
  }
  if (ips.empty()) {
    return true;
  }
  if (!show_callchain_) {
    ips.resize(1);
    kernel_ip_count = std::min(kernel_ip_count, static_cast<size_t>(1u));
  }
  sample_count_++;
  std::vector<CallEntry> entries;
  bool near_java_method = false;
  auto is_entry_for_interpreter = [](const CallEntry& entry) {
    return android::base::EndsWith(entry.dso->Path(), "/libart.so");
  };
  const ThreadEntry* thread = thread_tree_.FindThreadOrNew(r.tid_data.pid, r.tid_data.tid);
  for (size_t i = 0; i < ips.size(); ++i) {
    bool omit_unknown_dso = i > 0u;
    CallEntry entry;
    if (!GetCallEntry(thread, i < kernel_ip_count, ips[i], omit_unknown_dso, &entry)) {
      break;
    }
    if (!show_art_frames_) {
      // Remove interpreter frames both before and after the Java frame.
      if (entry.dso->type() == DSO_DEX_FILE) {
        near_java_method = true;
        while (!entries.empty() && is_entry_for_interpreter(entries.back())) {
          entries.pop_back();
        }
      } else if (is_entry_for_interpreter(entry)) {
        if (near_java_method) {
          continue;
        }
      } else {
        near_java_method = false;
      }
    }
    entries.push_back(entry);
  }
  if (use_protobuf_) {
    return PrintSampleRecordInProtobuf(r, entries);
  }
  return PrintSampleRecord(r, entries);
}

bool ReportSampleCommand::PrintSampleRecordInProtobuf(const SampleRecord& r,
                                                      const std::vector<CallEntry>& entries) {
  proto::Record proto_record;
  proto::Sample* sample = proto_record.mutable_sample();
  sample->set_time(r.time_data.time);
  sample->set_event_count(r.period_data.period);
  sample->set_thread_id(r.tid_data.tid);
  sample->set_event_type_id(record_file_reader_->GetAttrIndexOfRecord(&r));

  for (const CallEntry& node : entries) {
    proto::Sample_CallChainEntry* callchain = sample->add_callchain();
    uint32_t file_id;
    if (!node.dso->GetDumpId(&file_id)) {
      file_id = node.dso->CreateDumpId();
    }
    int32_t symbol_id = -1;
    if (node.symbol != thread_tree_.UnknownSymbol()) {
      if (!node.symbol->GetDumpId(reinterpret_cast<uint32_t*>(&symbol_id))) {
        symbol_id = node.dso->CreateSymbolDumpId(node.symbol);
      }
    }
    callchain->set_vaddr_in_file(node.vaddr_in_file);
    callchain->set_file_id(file_id);
    callchain->set_symbol_id(symbol_id);

    // Android studio wants a clear call chain end to notify whether a call chain is complete.
    // For the main thread, the call chain ends at __libc_init in libc.so. For other threads,
    // the call chain ends at __start_thread in libc.so.
    // The call chain of the main thread can go beyond __libc_init, to _start (<= android O) or
    // _start_main (> android O).
    if (node.dso->FileName() == "libc.so" &&
        (strcmp(node.symbol->Name(), "__libc_init") == 0 ||
            strcmp(node.symbol->Name(), "__start_thread") == 0)) {
      break;
    }
  }
  return WriteRecordInProtobuf(proto_record);
}

bool ReportSampleCommand::WriteRecordInProtobuf(proto::Record& proto_record) {
  coded_os_->WriteLittleEndian32(proto_record.ByteSize());
  if (!proto_record.SerializeToCodedStream(coded_os_)) {
    LOG(ERROR) << "failed to write record to protobuf";
    return false;
  }
  return true;
}

bool ReportSampleCommand::GetCallEntry(const ThreadEntry* thread,
                                       bool in_kernel, uint64_t ip,
                                       bool omit_unknown_dso,
                                       CallEntry* entry) {
  const MapEntry* map = thread_tree_.FindMap(thread, ip, in_kernel);
  if (omit_unknown_dso && thread_tree_.IsUnknownDso(map->dso)) {
    return false;
  }
  entry->symbol = thread_tree_.FindSymbol(map, ip, &(entry->vaddr_in_file), &(entry->dso));
  // If we can't find symbol, use the dso shown in the map.
  if (entry->symbol == thread_tree_.UnknownSymbol()) {
    entry->dso = map->dso;
  }
  return true;
}

bool ReportSampleCommand::PrintLostSituationInProtobuf() {
  proto::Record proto_record;
  proto::LostSituation* lost = proto_record.mutable_lost();
  lost->set_sample_count(sample_count_);
  lost->set_lost_count(lost_count_);
  return WriteRecordInProtobuf(proto_record);
}

static bool CompareDsoByDumpId(Dso* d1, Dso* d2) {
  uint32_t id1 = UINT_MAX;
  d1->GetDumpId(&id1);
  uint32_t id2 = UINT_MAX;
  d2->GetDumpId(&id2);
  return id1 < id2;
}

bool ReportSampleCommand::PrintFileInfoInProtobuf() {
  std::vector<Dso*> dsos = thread_tree_.GetAllDsos();
  std::sort(dsos.begin(), dsos.end(), CompareDsoByDumpId);
  for (Dso* dso : dsos) {
    uint32_t file_id;
    if (!dso->GetDumpId(&file_id)) {
      continue;
    }
    proto::Record proto_record;
    proto::File* file = proto_record.mutable_file();
    file->set_id(file_id);
    file->set_path(dso->Path());
    const std::vector<Symbol>& symbols = dso->GetSymbols();
    std::vector<const Symbol*> dump_symbols;
    for (const auto& sym : symbols) {
      if (sym.HasDumpId()) {
        dump_symbols.push_back(&sym);
      }
    }
    std::sort(dump_symbols.begin(), dump_symbols.end(),
              Symbol::CompareByDumpId);

    for (const auto& sym : dump_symbols) {
      std::string* symbol = file->add_symbol();
      *symbol = sym->DemangledName();
      std::string* mangled_symbol = file->add_mangled_symbol();
      *mangled_symbol = sym->Name();
    }
    if (!WriteRecordInProtobuf(proto_record)) {
      return false;
    }
  }
  return true;
}

bool ReportSampleCommand::PrintThreadInfoInProtobuf() {
  std::vector<const ThreadEntry*> threads = thread_tree_.GetAllThreads();
  auto compare_thread_id = [](const ThreadEntry* t1, const ThreadEntry* t2) {
    return t1->tid < t2->tid;
  };
  std::sort(threads.begin(), threads.end(), compare_thread_id);
  for (auto& thread : threads) {
    proto::Record proto_record;
    proto::Thread* proto_thread = proto_record.mutable_thread();
    proto_thread->set_thread_id(thread->tid);
    proto_thread->set_process_id(thread->pid);
    proto_thread->set_thread_name(thread->comm);
    if (!WriteRecordInProtobuf(proto_record)) {
      return false;
    }
  }
  return true;
}

bool ReportSampleCommand::PrintSampleRecord(const SampleRecord& r,
                                            const std::vector<CallEntry>& entries) {
  FprintIndented(report_fp_, 0, "sample:\n");
  FprintIndented(report_fp_, 1, "event_type: %s\n",
                 event_types_[record_file_reader_->GetAttrIndexOfRecord(&r)].c_str());
  FprintIndented(report_fp_, 1, "time: %" PRIu64 "\n", r.time_data.time);
  FprintIndented(report_fp_, 1, "event_count: %" PRIu64 "\n", r.period_data.period);
  FprintIndented(report_fp_, 1, "thread_id: %d\n", r.tid_data.tid);
  const char* thread_name = thread_tree_.FindThreadOrNew(r.tid_data.pid, r.tid_data.tid)->comm;
  FprintIndented(report_fp_, 1, "thread_name: %s\n", thread_name);
  CHECK(!entries.empty());
  FprintIndented(report_fp_, 1, "vaddr_in_file: %" PRIx64 "\n", entries[0].vaddr_in_file);
  FprintIndented(report_fp_, 1, "file: %s\n", entries[0].dso->Path().c_str());
  FprintIndented(report_fp_, 1, "symbol: %s\n", entries[0].symbol->DemangledName());

  if (entries.size() > 1u) {
    FprintIndented(report_fp_, 1, "callchain:\n");
    for (size_t i = 1u; i < entries.size(); ++i) {
      FprintIndented(report_fp_, 2, "vaddr_in_file: %" PRIx64 "\n", entries[i].vaddr_in_file);
      FprintIndented(report_fp_, 2, "file: %s\n", entries[i].dso->Path().c_str());
      FprintIndented(report_fp_, 2, "symbol: %s\n", entries[i].symbol->DemangledName());
    }
  }
  return true;
}

void ReportSampleCommand::PrintLostSituation() {
  FprintIndented(report_fp_, 0, "lost_situation:\n");
  FprintIndented(report_fp_, 1, "sample_count: %" PRIu64 "\n", sample_count_);
  FprintIndented(report_fp_, 1, "lost_count: %" PRIu64 "\n", lost_count_);
}

}  // namespace

void RegisterReportSampleCommand() {
  RegisterCommand("report-sample", [] {
    return std::unique_ptr<Command>(new ReportSampleCommand());
  });
}
