// Copyright 2016 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <getopt.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/types.h>

#include <algorithm>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include <base/bind.h>
#include <base/callback_forward.h>
#include <base/command_line.h>
#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/files/scoped_file.h>
#include <base/json/json_writer.h>
#include <base/macros.h>
#include <base/memory/ptr_util.h>
#include <base/posix/eintr_wrapper.h>
#include <base/process/launch.h>
#include <base/strings/string_number_conversions.h>
#include <base/strings/string_util.h>
#include <base/strings/stringprintf.h>

#include <libcontainer/libcontainer.h>

#include "run_oci/container_config_parser.h"
#include "run_oci/container_options.h"
#include "run_oci/run_oci_utils.h"

namespace {

using run_oci::BindMount;
using run_oci::BindMounts;
using run_oci::ContainerOptions;
using run_oci::OciConfigPtr;

using ContainerConfigPtr = std::unique_ptr<container_config,
                                           decltype(&container_config_destroy)>;

constexpr base::FilePath::CharType kRunContainersPath[] =
    FILE_PATH_LITERAL("/run/containers");

constexpr base::FilePath::CharType kProcSelfMountsPath[] =
    FILE_PATH_LITERAL("/proc/self/mounts");

constexpr base::FilePath::CharType kContainerPidFilename[] =
    FILE_PATH_LITERAL("container.pid");
constexpr base::FilePath::CharType kConfigJsonFilename[] =
    FILE_PATH_LITERAL("config.json");
constexpr base::FilePath::CharType kRunOciFilename[] =
    FILE_PATH_LITERAL(".run_oci");

// PIDs can be up to 8 characters, plus the terminating NUL byte. Rounding it up
// to the next power-of-two.
constexpr size_t kMaxPidFileLength = 16;

const std::map<std::string, int> kSignalMap = {
#define SIGNAL_MAP_ENTRY(name) \
  { #name, SIG##name }
    SIGNAL_MAP_ENTRY(HUP),   SIGNAL_MAP_ENTRY(INT),    SIGNAL_MAP_ENTRY(QUIT),
    SIGNAL_MAP_ENTRY(ILL),   SIGNAL_MAP_ENTRY(TRAP),   SIGNAL_MAP_ENTRY(ABRT),
    SIGNAL_MAP_ENTRY(BUS),   SIGNAL_MAP_ENTRY(FPE),    SIGNAL_MAP_ENTRY(KILL),
    SIGNAL_MAP_ENTRY(USR1),  SIGNAL_MAP_ENTRY(SEGV),   SIGNAL_MAP_ENTRY(USR2),
    SIGNAL_MAP_ENTRY(PIPE),  SIGNAL_MAP_ENTRY(ALRM),   SIGNAL_MAP_ENTRY(TERM),
    SIGNAL_MAP_ENTRY(CLD),   SIGNAL_MAP_ENTRY(CHLD),   SIGNAL_MAP_ENTRY(CONT),
    SIGNAL_MAP_ENTRY(STOP),  SIGNAL_MAP_ENTRY(TSTP),   SIGNAL_MAP_ENTRY(TTIN),
    SIGNAL_MAP_ENTRY(TTOU),  SIGNAL_MAP_ENTRY(URG),    SIGNAL_MAP_ENTRY(XCPU),
    SIGNAL_MAP_ENTRY(XFSZ),  SIGNAL_MAP_ENTRY(VTALRM), SIGNAL_MAP_ENTRY(PROF),
    SIGNAL_MAP_ENTRY(WINCH), SIGNAL_MAP_ENTRY(POLL),   SIGNAL_MAP_ENTRY(IO),
    SIGNAL_MAP_ENTRY(PWR),   SIGNAL_MAP_ENTRY(SYS),
#undef SIGNAL_MAP_ENTRY
};

// WaitablePipe provides a way for one process to wait on another. This only
// uses the read(2) and close(2) syscalls, so it can work even in a restrictive
// environment. Each process must call only one of Wait() and Signal() exactly
// once.
struct WaitablePipe {
  WaitablePipe() {
    if (pipe(pipe_fds) == -1)
      PLOG(FATAL) << "Failed to create pipe";
  }

  ~WaitablePipe() {
    if (pipe_fds[0] != -1)
      close(pipe_fds[0]);
    if (pipe_fds[1] != -1)
      close(pipe_fds[1]);
  }

  void Wait() {
    char buf;

    close(pipe_fds[1]);
    HANDLE_EINTR(read(pipe_fds[0], &buf, sizeof(buf)));
    close(pipe_fds[0]);

    pipe_fds[0] = -1;
    pipe_fds[1] = -1;
  }

  void Signal() {
    close(pipe_fds[0]);
    close(pipe_fds[1]);

    pipe_fds[0] = -1;
    pipe_fds[1] = -1;
  }

  int pipe_fds[2];
};

// PreStartHookState holds two WaitablePipes so that the container can wait for
// its parent to run prestart hooks just prior to calling execve(2).
struct PreStartHookState {
  WaitablePipe reached_pipe;
  WaitablePipe ready_pipe;
};

// DeferredRunner ensures a base::Closure is run when it goes out of scope.
class DeferredRunner {
 public:
  explicit DeferredRunner(base::Closure closure = base::Closure())
      : closure_(std::move(closure)) {}

  ~DeferredRunner() {
    if (closure_.is_null())
      return;
    closure_.Run();
  }

  void Reset(base::Closure closure = base::Closure()) {
    closure_ = std::move(closure);
  }

 private:
  base::Closure closure_;
  DISALLOW_COPY_AND_ASSIGN(DeferredRunner);
};

std::ostream& operator<<(std::ostream& o, const OciHook& hook) {
  o << "Hook{path=\"" << hook.path << "\", args=[";
  bool first = true;
  for (const auto& arg : hook.args) {
    if (!first)
      o << ", ";
    first = false;
    o << arg;
  }
  o << "]}";
  return o;
}

// Converts a single UID map to a string.
std::string GetIdMapString(const OciLinuxNamespaceMapping& map) {
  std::ostringstream oss;
  oss << map.containerID << " " << map.hostID << " " << map.size;
  return oss.str();
}

// Converts an array of UID mappings given in |maps| to the string format the
// kernel understands and puts that string in |map_string_out|.
std::string IdStringFromMap(const std::vector<OciLinuxNamespaceMapping>& maps) {
  std::ostringstream oss;
  bool first = true;
  for (const auto& map : maps) {
    if (first)
      first = false;
    else
      oss << ",";
    oss << GetIdMapString(map);
  }
  return oss.str();
}

// Parses the options from the OCI mount in to either mount flags in |flags_out|
// or a data string for mount(2) in |option_string_out|.
std::string ParseMountOptions(const std::vector<std::string>& options,
                              int* flags_out, int* loopback_out,
                              std::string *verity_options) {
  std::string option_string_out;
  *flags_out = 0;
  *loopback_out = 0;

  for (const auto& option : options) {
    if (option == "nodev") {
      *flags_out |= MS_NODEV;
    } else if (option == "noexec") {
      *flags_out |= MS_NOEXEC;
    } else if (option == "nosuid") {
      *flags_out |= MS_NOSUID;
    } else if (option == "bind") {
      *flags_out |= MS_BIND;
    } else if (option == "ro") {
      *flags_out |= MS_RDONLY;
    } else if (option == "private") {
      *flags_out |= MS_PRIVATE;
    } else if (option == "recursive") {
      *flags_out |= MS_REC;
    } else if (option == "slave") {
      *flags_out |= MS_SLAVE;
    } else if (option == "remount") {
      *flags_out |= MS_REMOUNT;
    } else if (option == "loop") {
      *loopback_out = 1;
    } else if (base::StartsWith(option, "dm=", base::CompareCase::SENSITIVE)) {
      *verity_options = option.substr(3, std::string::npos);
    } else {
      // Unknown options get appended to the string passed to mount data.
      if (!option_string_out.empty())
        option_string_out += ",";
      option_string_out += option;
    }
  }

  return option_string_out;
}

// Sanitize |flags| that can be used for filesystem of a given |type|.
int SanitizeFlags(const std::string &type, int flags) {
    int sanitized_flags = flags;
    // Right now, only sanitize sysfs and procfs.
    if (type != "sysfs" && type != "proc")
        return flags;

    // sysfs and proc should always have nodev, noexec, nosuid.
    // Warn the user if these weren't specified, then turn them on.
    sanitized_flags |= (MS_NODEV | MS_NOEXEC | MS_NOSUID);
    if (flags ^ sanitized_flags)
        LOG(WARNING) << "Sanitized mount of type " << type << ".";

    return sanitized_flags;
}

// Adds the mounts specified in |mounts| to |config_out|.
void ConfigureMounts(const std::vector<OciMount>& mounts,
                     uid_t uid, gid_t gid,
                     container_config* config_out) {
  for (const auto& mount : mounts) {
    int flags, loopback;
    std::string verity_options;
    std::string options = ParseMountOptions(mount.options, &flags, &loopback,
                                            &verity_options);
    flags = SanitizeFlags(mount.type, flags);

    container_config_add_mount(
        config_out,
        "mount",
        base::MakeAbsoluteFilePath(mount.source).value().c_str(),
        mount.destination.value().c_str(),
        mount.type.c_str(),
        options.empty() ? NULL : options.c_str(),
        verity_options.empty() ? NULL : verity_options.c_str(),
        flags,
        uid,
        gid,
        0750,
        // Loopback devices have to be mounted outside.
        !loopback,
        1,
        loopback);
  }
}

// Adds the devices specified in |devices| to |config_out|.
void ConfigureDevices(const std::vector<OciLinuxDevice>& devices,
                      container_config* config_out) {
  for (const auto& device : devices) {
    container_config_add_device(config_out,
                                device.type.c_str()[0],
                                device.path.c_str(),
                                device.fileMode,
                                device.major,
                                device.minor,
                                0,
                                device.uid,
                                device.gid,
                                0,  // Cgroup permission are now in 'resources'.
                                0,
                                0);
  }
}

// Adds the cgroup device permissions specified in |devices| to |config_out|.
void ConfigureCgroupDevices(const std::vector<OciLinuxCgroupDevice>& devices,
                            container_config* config_out) {
  for (const auto& device : devices) {
    bool read_set = device.access.find('r') != std::string::npos;
    bool write_set = device.access.find('w') != std::string::npos;
    bool make_set = device.access.find('m') != std::string::npos;
    container_config_add_cgroup_device(config_out,
                                       device.allow,
                                       device.type.c_str()[0],
                                       device.major,
                                       device.minor,
                                       read_set,
                                       write_set,
                                       make_set);
  }
}

// Fills the libcontainer container_config struct given in |config_out| by
// pulling the apropriate fields from the OCI configuration given in |oci|.
bool ContainerConfigFromOci(const OciConfig& oci,
                            const base::FilePath& container_root,
                            const std::vector<std::string>& extra_args,
                            container_config* config_out) {
  // Process configuration
  container_config_config_root(config_out, container_root.value().c_str());
  container_config_uid(config_out, oci.process.user.uid);
  container_config_gid(config_out, oci.process.user.gid);
  base::FilePath root_dir = container_root.Append(oci.root.path);
  container_config_premounted_runfs(config_out, root_dir.value().c_str());

  std::vector<const char *> argv;
  for (const auto& arg : oci.process.args)
    argv.push_back(arg.c_str());
  for (const auto& arg : extra_args)
    argv.push_back(arg.c_str());
  container_config_program_argv(config_out, argv.data(), argv.size());

  std::string uid_maps = IdStringFromMap(oci.linux_config.uidMappings);
  container_config_uid_map(config_out, uid_maps.c_str());
  std::string gid_maps = IdStringFromMap(oci.linux_config.gidMappings);
  container_config_gid_map(config_out, gid_maps.c_str());

  ConfigureMounts(oci.mounts, oci.process.user.uid,
                  oci.process.user.gid, config_out);
  ConfigureDevices(oci.linux_config.devices, config_out);
  ConfigureCgroupDevices(oci.linux_config.resources.devices, config_out);

  for (const auto& limit : oci.process.rlimits) {
    if (container_config_add_rlimit(
            config_out, limit.type, limit.soft, limit.hard)) {
      return false;
    }
  }

  return true;
}

// Reads json configuration of a container from |config_path| and filles
// |oci_out| with the specified container configuration.
bool OciConfigFromFile(const base::FilePath& config_path,
                       const OciConfigPtr& oci_out) {
  std::string config_json_data;
  if (!base::ReadFileToString(config_path, &config_json_data)) {
    PLOG(ERROR) << "Fail to config container.";
    return false;
  }

  if (!run_oci::ParseContainerConfig(config_json_data, oci_out)) {
    LOG(ERROR) << "Fail to parse config.json.";
    return false;
  }

  return true;
}

// Appends additional mounts specified in |bind_mounts| to the configuration
// given in |config_out|.
bool AppendMounts(const BindMounts& bind_mounts, container_config* config_out) {
  for (auto & mount : bind_mounts) {
    if (container_config_add_mount(config_out,
                                   "mount",
                                   mount.first.value().c_str(),
                                   mount.second.value().c_str(),
                                   "bind",
                                   NULL,
                                   NULL,
                                   MS_MGC_VAL | MS_BIND,
                                   0,
                                   0,
                                   0750,
                                   1,
                                   1,
                                   0)) {
      PLOG(ERROR) << "Failed to add mount of " << mount.first.value();
      return false;
    }
  }

  return true;
}

// Generates OCI-compliant, JSON-formatted container state. This is
// pretty-printed so that bash scripts can more easily grab the fields instead
// of having to parse the JSON blob.
std::string ContainerState(int child_pid,
                           const base::FilePath& bundle_dir,
                           const base::FilePath& container_dir,
                           const std::string& status) {
  base::DictionaryValue state;
  state.SetString("ociVersion", "1.0");
  state.SetString("id", base::StringPrintf("run_oci:%d", child_pid));
  state.SetString("status", status);
  state.SetString("bundle", base::MakeAbsoluteFilePath(bundle_dir).value());
  state.SetInteger("pid", child_pid);
  std::unique_ptr<base::DictionaryValue> annotations =
      base::MakeUnique<base::DictionaryValue>();
  annotations->SetStringWithoutPathExpansion(
      "org.chromium.run_oci.container_root",
      base::MakeAbsoluteFilePath(container_dir).value());
  state.Set("annotations", std::move(annotations));
  std::string state_json;
  if (!base::JSONWriter::WriteWithOptions(
          state, base::JSONWriter::OPTIONS_PRETTY_PRINT, &state_json)) {
    LOG(ERROR) << "Failed to serialize the container state";
    return std::string();
  }
  return state_json;
}

// Runs one hook.
bool RunOneHook(const OciHook& hook,
                const std::string& hook_type,
                const std::string& container_state) {
  base::LaunchOptions options;
  if (!hook.env.empty()) {
    options.clear_environ = true;
    options.environ = hook.env;
  }

  int write_pipe_fds[2] = {0};
  if (HANDLE_EINTR(pipe(write_pipe_fds)) != 0) {
    LOG(ERROR) << "Bad write pipe";
    return false;
  }
  base::ScopedFD write_pipe_read_fd(write_pipe_fds[0]);
  base::ScopedFD write_pipe_write_fd(write_pipe_fds[1]);
  base::FileHandleMappingVector fds_to_remap{
      {write_pipe_read_fd.get(), STDIN_FILENO}, {STDERR_FILENO, STDERR_FILENO}};
  options.fds_to_remap = &fds_to_remap;

  std::vector<std::string> args;
  if (hook.args.empty()) {
    args.push_back(hook.path);
  } else {
    args = hook.args;
    // Overwrite the first argument with the path since base::LaunchProcess does
    // not take an additional parameter for the executable name. Since the OCI
    // spec mandates that the path should be absolute, it's better to use that
    // rather than rely on whatever short name was passed in |args|.
    args[0] = hook.path;
  }

  DVLOG(1) << "Running " << hook_type << " " << hook;

  base::Process child = base::LaunchProcess(args, options);
  write_pipe_read_fd.reset();
  if (!base::WriteFileDescriptor(write_pipe_write_fd.get(),
                                 container_state.c_str(),
                                 container_state.size())) {
    PLOG(ERROR) << "Failed to send container state";
  }
  write_pipe_write_fd.reset();
  int exit_code;
  if (!child.WaitForExitWithTimeout(hook.timeout, &exit_code)) {
    LOG(ERROR) << "Timeout exceeded running " << hook_type << " hook " << hook;
    if (!child.Terminate(0, true))
      LOG(ERROR) << "Failed to terminate " << hook_type << " hook " << hook;
    return false;
  }
  if (exit_code != 0) {
    LOG(ERROR) << hook_type << " hook " << hook << " exited with status "
               << exit_code;
    return false;
  }
  return true;
}

bool RunHooks(const std::vector<OciHook>& hooks,
              int child_pid,
              const base::FilePath& bundle_dir,
              const base::FilePath& container_dir,
              const std::string& hook_stage,
              const std::string& status) {
  bool success = true;
  std::string container_state =
      ContainerState(child_pid, bundle_dir, container_dir, status);
  for (const auto& hook : hooks)
    success &= RunOneHook(hook, hook_stage, container_state);
  return success;
}

void RunPostStopHooks(const std::vector<OciHook>& hooks,
                      int child_pid,
                      const base::FilePath& bundle_dir,
                      const base::FilePath& container_dir) {
  if (!RunHooks(
          hooks, child_pid, bundle_dir, container_dir, "poststop", "stopped")) {
    LOG(WARNING) << "Error running poststop hooks";
  }
}

// A callback that is run in the container process just before calling execve(2)
// that waits for the parent process to run all the prestart hooks.
int WaitForPreStartHooks(void* payload) {
  PreStartHookState* state = reinterpret_cast<PreStartHookState*>(payload);

  state->reached_pipe.Signal();
  state->ready_pipe.Wait();

  return 0;
}

void CleanUpContainer(const base::FilePath& container_dir) {
  std::vector<base::FilePath> mountpoints = run_oci::GetMountpointsUnder(
      container_dir, base::FilePath(kProcSelfMountsPath));

  // Sort the list of mountpoints. Since this is a tree structure, unmounting
  // recursively can be achieved by traversing this list in inverse
  // lexicographic order.
  std::sort(
      mountpoints.begin(),
      mountpoints.end(),
      [](const base::FilePath& a, const base::FilePath& b) { return b < a; });
  for (const auto& mountpoint : mountpoints) {
    if (umount2(mountpoint.value().c_str(), MNT_DETACH))
      PLOG(ERROR) << "Failed to unmount " << mountpoint.value();
  }

  if (!base::DeleteFile(container_dir, true /*recursive*/)) {
    PLOG(ERROR) << "Failed to clean up the container directory "
                << container_dir.value();
  }
}

// Runs an OCI image with the configuration found at |bundle_dir|.
// If |inplace| is true, |bundle_dir| will also be used to mount the rootfs.
// Otherwise, a new directory under kRunContainersPath will be created.
// If |detach_after_start| is true, blocks until the program specified in
// config.json exits, otherwise blocks until after the post-start hooks have
// finished.
// Returns -1 on error.
int RunOci(const base::FilePath& bundle_dir,
           const std::string& container_id,
           const ContainerOptions& container_options,
           bool inplace,
           bool detach_after_start) {
  base::FilePath container_dir;
  const base::FilePath container_config_file =
      bundle_dir.Append(kConfigJsonFilename);

  OciConfigPtr oci_config(new OciConfig());
  if (!OciConfigFromFile(container_config_file, oci_config)) {
    return -1;
  }

  DeferredRunner cleanup;

  if (detach_after_start) {
    container_dir = base::FilePath(kRunContainersPath).Append(container_id);
    if (inplace) {
      if (container_dir != bundle_dir) {
        LOG(ERROR) << "With --inplace, the directory where config.json is "
                      "located must be "
                   << container_dir.value();
        return -1;
      }
    } else {
      LOG(ERROR)
          << "Non-inplace mode not implemented yet. Please pass in --inplace.";
      return -1;
    }

    cleanup.Reset(base::Bind(CleanUpContainer, container_dir));

    // Create an empty file, just to tag this container as being
    // run_oci-managed.
    if (base::WriteFile(container_dir.Append(kRunOciFilename), "", 0) != 0) {
      LOG(ERROR) << "Failed to create .run_oci tag file";
      return -1;
    }
  } else {
    container_dir = bundle_dir;
  }

  ContainerConfigPtr config(container_config_create(),
                            &container_config_destroy);
  if (!ContainerConfigFromOci(*oci_config,
                              container_dir,
                              container_options.extra_program_args,
                              config.get())) {
    PLOG(ERROR) << "Failed to create container from oci config.";
    return -1;
  }

  AppendMounts(container_options.bind_mounts, config.get());
  // Create a container based on the config.  The run_dir argument will be
  // unused as this container will be run in place where it was mounted.
  std::unique_ptr<container, decltype(&container_destroy)>
      container(container_new(oci_config->hostname.c_str(), "/unused"),
                &container_destroy);

  container_config_keep_fds_open(config.get());
  if (!oci_config->process.capabilities.empty()) {
    container_config_set_capmask(
        config.get(),
        oci_config->process.capabilities["effective"].to_ullong(),
        oci_config->process.capabilities.find("ambient") !=
            oci_config->process.capabilities.end());
  }

  if (!oci_config->process.selinuxLabel.empty()) {
    container_config_set_selinux_context(
        config.get(), oci_config->process.selinuxLabel.c_str());
  }

  std::unique_ptr<PreStartHookState> pre_start_hook_state;
  if (!oci_config->pre_start_hooks.empty()) {
    pre_start_hook_state = base::MakeUnique<PreStartHookState>();
    int inherited_fds[4];
    memcpy(inherited_fds,
           pre_start_hook_state->reached_pipe.pipe_fds,
           sizeof(pre_start_hook_state->reached_pipe.pipe_fds));
    memcpy(
        inherited_fds + arraysize(pre_start_hook_state->reached_pipe.pipe_fds),
        pre_start_hook_state->ready_pipe.pipe_fds,
        sizeof(pre_start_hook_state->ready_pipe.pipe_fds));
    // All these fds will be closed in WaitForPreStartHooks in the child
    // process.
    container_config_inherit_fds(
        config.get(), inherited_fds, arraysize(inherited_fds));
    container_config_set_pre_execve_hook(config.get(),
                                        WaitForPreStartHooks,
                                        pre_start_hook_state.get());
  }

  if (container_options.cgroup_parent.length() > 0) {
    container_config_set_cgroup_parent(config.get(),
                                       container_options.cgroup_parent.c_str(),
                                       container_config_get_uid(config.get()),
                                       container_config_get_gid(config.get()));
  }

  if (container_options.use_current_user) {
    OciLinuxNamespaceMapping single_map = {
      getuid(),  // hostID
      0,         // containerID
      1          // size
    };
    std::string map_string = GetIdMapString(single_map);
    container_config_uid_map(config.get(), map_string.c_str());
    container_config_gid_map(config.get(), map_string.c_str());
  }

  if (!container_options.alt_syscall_table.empty()) {
    container_config_alt_syscall_table(
        config.get(), container_options.alt_syscall_table.c_str());
  }

  if (container_options.securebits_skip_mask) {
    container_config_set_securebits_skip_mask(
        config.get(), container_options.securebits_skip_mask);
  }

  container_config_set_run_as_init(config.get(), container_options.run_as_init);

  int rc;
  rc = container_start(container.get(), config.get());
  if (rc) {
    PLOG(ERROR) << "start failed.";
    return -1;
  }

  int child_pid = container_pid(container.get());
  if (detach_after_start) {
    const base::FilePath container_pid_path =
        container_dir.Append(kContainerPidFilename);
    std::string child_pid_str = base::StringPrintf("%d\n", child_pid);
    if (base::WriteFile(container_pid_path,
                        child_pid_str.c_str(),
                        child_pid_str.size()) != child_pid_str.size()) {
      PLOG(ERROR) << "Failed to write the container PID to "
                  << container_pid_path.value();
      return -1;
    }
  }

  // The callback is run in the same stack, so base::ConstRef() is safe.
  DeferredRunner post_stop_hooks(
      base::Bind(&RunPostStopHooks,
                 base::ConstRef(oci_config->post_stop_hooks),
                 child_pid,
                 bundle_dir,
                 container_dir));

  if (pre_start_hook_state) {
    pre_start_hook_state->reached_pipe.Wait();
    if (!RunHooks(oci_config->pre_start_hooks,
                  child_pid,
                  bundle_dir,
                  container_dir,
                  "prestart",
                  "created")) {
      LOG(ERROR) << "Failed to run all prestart hooks";
      container_kill(container.get());
      return -1;
    }
    pre_start_hook_state->ready_pipe.Signal();
  }

  if (!RunHooks(oci_config->post_start_hooks,
                child_pid,
                bundle_dir,
                container_dir,
                "poststart",
                "running")) {
    LOG(ERROR) << "Error running poststart hooks";
    container_kill(container.get());
    return -1;
  }

  if (detach_after_start) {
    // The container has reached a steady state. We can now return and let the
    // container keep running. We don't want to run the post-stop hooks now, but
    // until the user actually deletes the container.
    post_stop_hooks.Reset();
    cleanup.Reset();
    return 0;
  }

  return container_wait(container.get());
}

bool GetContainerPID(const std::string& container_id, pid_t* pid_out) {
  const base::FilePath container_dir =
      base::FilePath(kRunContainersPath).Append(container_id);
  const base::FilePath container_pid_path =
      container_dir.Append(kContainerPidFilename);

  std::string container_pid_str;
  if (!base::ReadFileToStringWithMaxSize(
          container_pid_path, &container_pid_str, kMaxPidFileLength)) {
    PLOG(ERROR) << "Failed to read " << container_pid_path.value();
    return false;
  }

  int container_pid;
  if (!base::StringToInt(
          base::TrimWhitespaceASCII(container_pid_str, base::TRIM_ALL),
          &container_pid)) {
    LOG(ERROR) << "Failed to convert the container pid to a number";
    return false;
  }

  if (!base::PathExists(container_dir.Append(kRunOciFilename))) {
    LOG(ERROR) << "Container " << container_id << " is not run_oci-managed";
    return false;
  }

  *pid_out = static_cast<pid_t>(container_pid);
  return true;
}

int OciKill(const std::string& container_id, int kill_signal) {
  pid_t container_pid;
  if (!GetContainerPID(container_id, &container_pid))
    return -1;

  if (kill(container_pid, kill_signal) == -1) {
    PLOG(ERROR) << "Failed to send signal";
    return -1;
  }

  return 0;
}

base::FilePath GetBundlePath(const base::FilePath& container_config_file) {
  if (!base::IsLink(container_config_file)) {
    // If the config.json is not a symlink, it was created using --inplace.
    return container_config_file.DirName();
  }
  base::FilePath bundle_path;
  if (!base::ReadSymbolicLink(container_config_file, &bundle_path)) {
    PLOG(ERROR) << "Failed to read symlink " << container_config_file.value();
    return base::FilePath();
  }
  return bundle_path.DirName();
}

int OciDestroy(const std::string& container_id) {
  const base::FilePath container_dir =
      base::FilePath(kRunContainersPath).Append(container_id);
  const base::FilePath container_config_file =
      container_dir.Append(kConfigJsonFilename);

  pid_t container_pid;
  if (!GetContainerPID(container_id, &container_pid))
    return -1;

  OciConfigPtr oci_config(new OciConfig());
  if (!OciConfigFromFile(container_config_file, oci_config)) {
    return -1;
  }

  if (kill(container_pid, 0) != -1 || errno != ESRCH) {
    PLOG(ERROR) << "Container " << container_id << " is still running.";
    return -1;
  }

  // We are committed to cleaning everything up now.
  RunPostStopHooks(oci_config->post_stop_hooks,
                   container_pid,
                   GetBundlePath(container_config_file),
                   container_dir);
  CleanUpContainer(container_dir);

  return 0;
}

const struct option longopts[] = {
  { "bind_mount", required_argument, NULL, 'b' },
  { "help", no_argument, NULL, 'h' },
  { "cgroup_parent", required_argument, NULL, 'p' },
  { "alt_syscall", required_argument, NULL, 's' },
  { "securebits_skip_mask", required_argument, NULL, 'B' },
  { "use_current_user", no_argument, NULL, 'u' },
  { "signal", required_argument, NULL, 'S' },
  { "container_path", required_argument, NULL, 'c' },
  { "inplace", no_argument, NULL, 128 },
  { 0, 0, 0, 0 },
};

void print_help(const char *argv0) {
  printf(
      "usage: %1$s [OPTIONS] <command> <container id>\n"
      "Commands:\n"
      "  run     creates and runs the container in the foreground.\n"
      "          %1$s will remain alive until the container's\n"
      "          init process exits and all resources are freed.\n"
      "          Running a container in this way does not support\n"
      "          the 'kill' or 'destroy' commands\n"
      "  start   creates and runs the container in the background.\n"
      "          The container can then be torn down with the 'kill'\n"
      "          command, and resources freed with the 'delete' command.\n"
      "  kill    sends the specified signal to the container's init.\n"
      "          the post-stop hooks will not be run at this time.\n"
      "  destroy runs the post-stop hooks and releases all resources.\n"
      "\n"
      "Global options:\n"
      "  -h, --help                     Print this message and exit.\n"
      "\n"
      "run/start:\n"
      "\n"
      "  %1$s {run,start} [OPTIONS] <container id> [-- <args>]\n"
      "\n"
      "Options for run and start:\n"
      "  -c, --container_path=<PATH>    The path of the container.\n"
      "                                 Defaults to $PWD.\n"
      "  -b, --bind_mount=<A>:<B>       Mount path A to B container.\n"
      "  -p, --cgroup_parent=<NAME>     Set parent cgroup for container.\n"
      "  -s, --alt_syscall=<NAME>       Set the alt-syscall table.\n"
      "  -B, --securebits_skip_mask=<MASK> Skips setting securebits in\n"
      "                                 <mask> when restricting caps.\n"
      "  -u, --use_current_user         Map the current user/group only.\n"
      "  -i, --dont_run_as_init         Do not run the command as init.\n"
      "\n"
      "Options for start:\n"
      "  --inplace                      The container path is the same\n"
      "                                 as the state path. Useful if the\n"
      "                                 config.json file needs to be\n"
      "                                 modified prior to running.\n"
      "\n"
      "kill:\n"
      "\n"
      "  %1$s kill [OPTIONS] <container id>\n"
      "\n"
      "Options for kill:\n"
      "  -S, --signal=<SIGNAL>          The signal to send to init.\n"
      "                                 Defaults to TERM.\n"
      "destroy:\n"
      "\n"
      "  %1$s destroy <container id>\n"
      "\n",
      argv0);
}

}  // anonymous namespace

int main(int argc, char **argv) {
  ContainerOptions container_options;
  base::FilePath bundle_dir = base::MakeAbsoluteFilePath(base::FilePath("."));
  int c;
  int kill_signal = SIGTERM;
  bool inplace = false;

  while ((c = getopt_long(argc, argv, "b:B:c:hp:s:S:uU", longopts, NULL)) !=
         -1) {
    switch (c) {
    case 'b': {
      std::istringstream ss(optarg);
      std::string outside_path;
      std::string inside_path;
      std::getline(ss, outside_path, ':');
      std::getline(ss, inside_path, ':');
      if (outside_path.empty() || inside_path.empty()) {
        print_help(argv[0]);
        return -1;
      }
      container_options.bind_mounts.push_back(
          BindMount(base::FilePath(outside_path), base::FilePath(inside_path)));
      break;
    }
    case 'B':
      if (!base::HexStringToUInt64(optarg,
                                   &container_options.securebits_skip_mask)) {
        print_help(argv[0]);
        return -1;
      }
      break;
    case 'c':
      bundle_dir = base::MakeAbsoluteFilePath(base::FilePath(optarg));
      break;
    case 'u':
      container_options.use_current_user = true;
      break;
    case 'p':
      container_options.cgroup_parent = optarg;
      break;
    case 's':
      container_options.alt_syscall_table = optarg;
      break;
    case 'S': {
      auto it = kSignalMap.find(optarg);
      if (it == kSignalMap.end()) {
        LOG(ERROR) << "Invalid signal name '" << optarg << "'";
        return -1;
      }
      kill_signal = it->second;
      break;
    }
    case 'i':
      container_options.run_as_init = false;
      break;
    case 'h':
      print_help(argv[0]);
      return 0;
    case 128:  // inplace
      inplace = true;
      break;
    default:
      print_help(argv[0]);
      return 1;
    }
  }

  if (optind >= argc) {
    LOG(ERROR) << "Command is required.";
    print_help(argv[0]);
    return -1;
  }
  std::string command(argv[optind++]);

  if (optind >= argc) {
    LOG(ERROR) << "Container id is required.";
    print_help(argv[0]);
    return -1;
  }
  std::string container_id(argv[optind++]);

  for (; optind < argc; optind++)
    container_options.extra_program_args.push_back(std::string(argv[optind]));

  if (command == "run") {
    return RunOci(bundle_dir,
                  container_id,
                  container_options,
                  true /*inplace*/,
                  false /*detach_after_start*/);
  } else if (command == "start") {
    return RunOci(bundle_dir,
                  container_id,
                  container_options,
                  inplace,
                  true /*detach_after_start*/);
  } else if (command == "kill") {
    return OciKill(container_id, kill_signal);
  } else if (command == "destroy") {
    return OciDestroy(container_id);
  } else {
    LOG(ERROR) << "Unknown command '" << command << "'";
    print_help(argv[0]);
    return -1;
  }
}
