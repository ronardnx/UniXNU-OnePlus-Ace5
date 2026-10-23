#!/usr/bin/env python3
"""Static check that the HFI allocation path and the stage machine are wired in.

The three HFI units (`giulia_gpu_hfi_alloc.h`, `..._pool.h`, `..._pump.h`) and
the stage framework are host tested, but the join with `GiuliaGPU.cpp` cannot be
compiled off-device. This check keeps that join honest: the members exist, the
pool is initialised only after the firmware memory is prepared, the pump runs
inside the worker, the stage plan is reachable from `gpu.stack`, and the stop
path never claims to have released pool memory.

It is a source-structure check, not a compilation: a green result means the
call sites and guards are present, not that the kernel links.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SERVICE = ROOT / "ios16" / "gpu" / "GiuliaGPU.cpp"
IMPL = ROOT / "ios16" / "gpu" / "giulia_gpu_hfi_alloc_impl.h"


def fail(message):
    print("HFI WIRING: " + message)
    raise SystemExit(1)


def require(text, needle, where):
    if needle not in text:
        fail("missing %r in %s" % (needle, where))


def block(lines, signature, where):
    """Line range of a function body, from its signature line to its closing brace."""
    start = None
    for index, line in enumerate(lines):
        if signature in line:
            start = index
            break
    if start is None:
        fail("missing %r in %s" % (signature, where))
    depth = 0
    opened = False
    for index in range(start, len(lines)):
        depth += lines[index].count("{") - lines[index].count("}")
        opened = opened or "{" in lines[index]
        if opened and depth <= 0:
            return start, index
    fail("unbalanced body for %r in %s" % (signature, where))


def main():
    if not SERVICE.is_file() or not IMPL.is_file():
        fail("service or implementation file missing")
    service = SERVICE.read_text(encoding="utf-8")
    impl = IMPL.read_text(encoding="utf-8")
    lines = service.splitlines()

    for header in ("giulia_gpu_hfi_alloc_pool.h", "giulia_gpu_hfi_pump.h",
                   "giulia_gpu_hfi_alloc_impl.h"):
        require(service, '#include "%s"' % header, "GiuliaGPU.cpp")

    class_line = next((i for i, line in enumerate(lines) if "OSDefineMetaClassAndStructors" in line), None)
    impl_line = next((i for i, line in enumerate(lines) if 'giulia_gpu_hfi_alloc_impl.h' in line), None)
    if class_line is None or impl_line is None or impl_line <= class_line:
        fail("the implementation header must be included after the class definition")
    if not lines[-1].strip().startswith("#endif"):
        fail("the service file must still end with the board-config guard")

    for member in ("struct gga_pool hfiAllocPool;", "struct gga_ops hfiAllocOps;",
                   "struct ggpp_state hfiPump;", "bool hfiAllocReady;"):
        require(service, member, "the GiuliaGPU class")

    start_begin, start_end = block(lines, "bool GiuliaGPU::start(", "GiuliaGPU.cpp")
    start_body = "\n".join(lines[start_begin:start_end + 1])
    require(start_body, "prepareMemory(&pack)", "start()")
    require(start_body, "hfiAllocInit();", "start()")
    if start_body.index("hfiAllocInit();") < start_body.index("prepareMemory(&pack)"):
        fail("the pool adapter must be initialised after the firmware memory is prepared")

    worker_begin, worker_end = block(lines, "void GiuliaGPU::worker(", "GiuliaGPU.cpp")
    worker_body = "\n".join(lines[worker_begin:worker_end + 1])
    require(worker_body, "hfiPumpOnce(", "worker()")
    require(worker_body, "gunyahPeriodic();", "worker()")

    report_begin, report_end = block(lines, "void GiuliaGPU::reportStatus()", "GiuliaGPU.cpp")
    require("\n".join(lines[report_begin:report_end + 1]), "hfiAllocReport();", "reportStatus()")

    stop_begin, stop_end = block(lines, "void GiuliaGPU::stop(", "GiuliaGPU.cpp")
    stop_body = "\n".join(lines[stop_begin:stop_end + 1])
    require(stop_body, "hfiPump.quarantined", "stop()")
    if "gga_pool_release(" in stop_body or "hfiAllocPool.failures=0" in stop_body:
        fail("stop() must not claim to release or reset pool state")

    for definition in ("uint64_t GiuliaGPU::hfiPoolAddress(",
                       "int GiuliaGPU::hfiPoolMap(",
                       "int GiuliaGPU::hfiPoolUnmap(",
                       "int GiuliaGPU::hfiPoolPublish(",
                       "int GiuliaGPU::hfiPoolBarrier(",
                       "void GiuliaGPU::hfiAllocInit()",
                       "int GiuliaGPU::hfiPumpOnce(",
                       "void GiuliaGPU::hfiAllocReport()"):
        require(impl, definition, "the implementation header")

    pump_begin, pump_end = block(impl.splitlines(), "int GiuliaGPU::hfiPumpOnce(", "impl")
    pump_body = "\n".join(impl.splitlines()[pump_begin:pump_end + 1])
    for guard in ("hfiAllocReady", "hostPrepared", "manualMemoryPinned",
                  "devMappingsManaged", "platformPoisoned", "gmuFaulted"):
        require(pump_body, guard, "hfiPumpOnce()")
    require(impl, "io.reserved=NULL", "hfiAllocInit()")
    if re.search(r"gga_pool_release\s*\(", impl):
        fail("the service must never call the responder's release path directly")

    print("HFI WIRING: members, init order, worker pump, report and stop path verified "
          "(source structure only, no compilation)")


def stack_wiring():
    terminal = (ROOT / "ios16" / "gpu" / "giulia_gpu_terminal.h").read_text(encoding="utf-8")
    require(terminal, "GGT_STACK", "giulia_gpu_terminal.h")
    require(terminal, 'gg_terminal_equal(command,"gpu.stack")', "giulia_gpu_terminal.h")
    service = SERVICE.read_text(encoding="utf-8")
    lines = service.splitlines()
    for header in ("giulia_gpu_stack.h", "giulia_gpu_stack_impl.h"):
        require(service, '#include "%s"' % header, "GiuliaGPU.cpp")
    for member in ("struct ggsk stackState;", "struct ggsk_ops stackOps[GGSK_STAGES];",
                   "bool stackReady;"):
        require(service, member, "the GiuliaGPU class")
    start_begin, start_end = block(lines, "bool GiuliaGPU::start(", "GiuliaGPU.cpp")
    start_body = "\n".join(lines[start_begin:start_end + 1])
    require(start_body, "stackInit();", "start()")
    if start_body.index("stackInit();") < start_body.index("prepareMemory(&pack)"):
        fail("the stage machine must be registered after the firmware memory is prepared")
    worker_begin, worker_end = block(lines, "void GiuliaGPU::worker(", "GiuliaGPU.cpp")
    worker_body = "\n".join(lines[worker_begin:worker_end + 1])
    require(worker_body, "GGT_STACK", "worker()")
    require(worker_body, "stackPlan()", "worker()")
    report_begin, report_end = block(lines, "void GiuliaGPU::reportStatus()", "GiuliaGPU.cpp")
    require("\n".join(lines[report_begin:report_end + 1]), "stackReport();", "reportStatus()")
    stop_begin, stop_end = block(lines, "void GiuliaGPU::stop(", "GiuliaGPU.cpp")
    require("\n".join(lines[stop_begin:stop_end + 1]), "ggsk_retreat", "stop()")
    impl = (ROOT / "ios16" / "gpu" / "giulia_gpu_stack_impl.h").read_text(encoding="utf-8")
    for definition in ("int GiuliaGPU::stackProbeBoot(", "int GiuliaGPU::stackProbeFirmware(",
                       "int GiuliaGPU::stackProbeMemory(", "void GiuliaGPU::stackInit()",
                       "int GiuliaGPU::stackPlan()", "void GiuliaGPU::stackReport()"):
        require(impl, definition, "giulia_gpu_stack_impl.h")
    require(impl, "policy.allow_device_writes=0", "stackInit()")
    require(impl, "GGSK_METAL", "stackPlan()")
    print("STACK WIRING: members, init order, gpu.stack command, report and retreat verified "
          "(source structure only, no compilation)")


def share_wiring():
    terminal = (ROOT / "ios16" / "gpu" / "giulia_gpu_terminal.h").read_text(encoding="utf-8")
    require(terminal, "GGT_SHARE", "giulia_gpu_terminal.h")
    require(terminal, 'gg_terminal_equal(command,"gpu.share")', "giulia_gpu_terminal.h")
    service = SERVICE.read_text(encoding="utf-8")
    lines = service.splitlines()
    for header in ("giulia_gpu_share_runtime.h", "giulia_gpu_share_impl.h"):
        require(service, '#include "%s"' % header, "GiuliaGPU.cpp")
    for member in ("struct ggsr_state shareRuntime;", "struct ggsh_owner shareOwner;",
                   "struct ggsr_gate shareGate;", "bool shareControlAllocate();"):
        require(service, member, "the GiuliaGPU class")
    worker_begin, worker_end = block(lines, "void GiuliaGPU::worker(", "GiuliaGPU.cpp")
    worker_body = "\n".join(lines[worker_begin:worker_end + 1])
    require(worker_body, "GGT_SHARE", "worker()")
    require(worker_body, "shareCommand()", "worker()")
    report_begin, report_end = block(lines, "void GiuliaGPU::reportStatus()", "GiuliaGPU.cpp")
    require("\n".join(lines[report_begin:report_end + 1]), "shareReport();", "reportStatus()")
    release_begin, release_end = block(lines, "void GiuliaGPU::releaseResources()", "GiuliaGPU.cpp")
    release_body = "\n".join(lines[release_begin:release_end + 1])
    require(release_body, "shareRuntime.prepared", "releaseResources()")
    impl = (ROOT / "ios16" / "gpu" / "giulia_gpu_share_impl.h").read_text(encoding="utf-8")
    for definition in ("int GiuliaGPU::shareLive(", "int GiuliaGPU::shareIdentity(",
                       "int GiuliaGPU::shareAuthorize(", "int GiuliaGPU::sharePublish(",
                       "int GiuliaGPU::shareCommand()", "void GiuliaGPU::shareReport()"):
        require(impl, definition, "giulia_gpu_share_impl.h")
    require(impl, "authority=unavailable", "shareAuthorize()")
    require(impl, "ggsr_prepare(", "shareCommand()")
    if "return 1;" in impl.split("int GiuliaGPU::shareAuthorize(")[1].split("}")[0]:
        fail("shareAuthorize() must not authorise anything without a real backend")
    require(service, "GGF_CARVEOUT_PHYSICAL", "shareControlAllocate()")
    print("SHARE WIRING: gpu.share command, owner/channel glue, control page and refusal path "
          "verified (source structure only, no compilation)")



if __name__ == "__main__":
    main()
    stack_wiring()
    share_wiring()
