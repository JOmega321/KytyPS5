#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Turn a fsr4fg-record trace into a replay contract for AMD's FSR frame generation 4.0.2.

Usage: python3 -I analyze_fg.py RUN_DIR DLL OUT.json

The contract holds what a native replay must reproduce: each frame's passes in order (debug label, entry point,
shader SHA-256, workgroup size, required subgroup size, group counts), every binding by its SPIR-V resource name
with the resource it was given, the constant buffers' exact contents, and the weights upload's place in the DLL.
It stores hashes, sizes and constants only; no shader or weight bytes. Standard library only.
"""
import hashlib
import json
import struct
import sys
from pathlib import Path

CHANNELS = {9: 1, 16: 2, 37: 4, 44: 4, 64: 4, 83: 2, 97: 4, 98: 1, 99: 1, 100: 1, 101: 2, 103: 2, 109: 4, 122: 3}
DESCRIPTOR_TYPES = {0: "sampler", 1: "combined_image_sampler", 2: "sampled_image", 3: "storage_image",
                    4: "uniform_texel_buffer", 5: "storage_texel_buffer", 6: "uniform_buffer", 7: "storage_buffer"}


def reflect(code):
    """Entry point, LocalSize and {(set, binding): name} of a SPIR-V module (names of the entry's interface only)."""
    if len(code) < 20 or len(code) % 4:
        raise ValueError("malformed SPIR-V")
    w = struct.unpack("<%dI" % (len(code) // 4), code)
    if w[0] != 0x07230203:
        raise ValueError("not SPIR-V")
    names, binding, dset = {}, {}, {}
    entry, local, interface = None, None, []
    at = 5
    while at < len(w):
        n, op = w[at] >> 16, w[at] & 0xFFFF
        if not n or at + n > len(w):
            raise ValueError("malformed SPIR-V instruction")
        a = w[at:at + n]
        if op == 15:
            raw = struct.pack("<%dI" % (n - 3), *a[3:])
            entry = raw.split(b"\0")[0].decode()
            interface = list(a[3 + (len(entry) + 4) // 4:])
        elif op == 16 and n >= 6 and a[2] == 17:
            local = list(a[3:6])
        elif op == 5:
            names[a[1]] = struct.pack("<%dI" % (n - 2), *a[2:]).split(b"\0")[0].decode(errors="replace")
        elif op == 71 and n >= 4 and a[2] == 33:
            binding[a[1]] = a[3]
        elif op == 71 and n >= 4 and a[2] == 34:
            dset[a[1]] = a[3]
        at += n
    by_slot = {}
    for var, b in binding.items():
        if var in interface:
            by_slot[(dset.get(var, 0), b)] = names.get(var, "?%d" % var)
    return entry, local, by_slot


def main():
    run, dll_path, out_path = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    dll = dll_path.read_bytes()
    ops = [json.loads(line) for line in (run / "trace.jsonl").read_text().splitlines()]
    notes = [o for o in ops if o["op"] == "note"]
    selected = next((n["name"] for n in notes if n.get("stage") == "selected"), None)
    if not selected:
        raise SystemExit("no provider was selected in this trace")
    for stage in ("create", "configure", "prepare", "generate", "destroy"):
        failed = [n for n in notes if n.get("stage") == stage and n.get("result")]
        if failed:
            raise SystemExit("%s failed: %s" % (stage, failed[0]))

    # Resources in creation order: stable names across runs (handles are not).
    resources, index = [], {}
    inputs = {n["image"]: n["name"] for n in notes if n.get("stage") == "input"}
    for o in ops:
        if o["op"] == "image":
            index[o["id"]] = len(resources)
            resources.append({"kind": "image", "format": o["format"], "size": [o["width"], o["height"], o["depth"]],
                              "mips": o["mips"], "layers": o["layers"], "usage": o["usage"],
                              "input": inputs.get(o["id"])})
        elif o["op"] == "buffer":
            index[o["id"]] = len(resources)
            resources.append({"kind": "buffer", "size": o["size"], "usage": o["usage"]})
    views = {o["id"]: o for o in ops if o["op"] == "image_view"}
    pipelines = {o["id"]: o for o in ops if o["op"] == "pipeline"}
    layouts = {o["id"]: o for o in ops if o["op"] == "pipeline_layout"}
    set_layouts = {o["id"]: o for o in ops if o["op"] == "set_layout"}
    samplers = {o["id"]: {k: v for k, v in o.items() if k not in ("op", "id")} for o in ops if o["op"] == "sampler"}

    def immutable_sampler(pid, dset, binding):
        layout = layouts[pipelines[pid]["layout"]]
        if dset >= len(layout["set_layouts"]):
            return None
        for b in set_layouts[layout["set_layouts"][dset]]["bindings"]:
            if b["binding"] == binding and b["immutable_samplers"]:
                return [samplers[s] for s in b["immutable_samplers"]]
        return None
    reflected, shader_sha = {}, {}

    def shader(pid):
        if pid not in reflected:
            stage = pipelines[pid]["stages"][0]
            code = (run / stage["shader"]).read_bytes()
            shader_sha[pid] = hashlib.sha256(code).hexdigest()
            if dll.find(code) < 0:
                raise SystemExit("shader %s is not a module of the DLL" % stage["shader"])
            reflected[pid] = reflect(code)
        return reflected[pid]

    constants = {}
    frames, label = {}, None
    for o in ops:
        if o["op"] == "label":
            label = o["name"]
        elif o["op"] == "dispatch_indirect":
            raise SystemExit("indirect dispatch: a replay would need the GPU's results")
        elif o["op"] == "dispatch":
            entry, local, slots = shader(o["pipeline"])
            if entry != o["entry"]:
                raise SystemExit("entry point mismatch")
            stage = pipelines[o["pipeline"]]["stages"][0]
            bindings = []
            for b in o["bindings"]:
                name = slots.get((b["set"], b["binding"]))
                if name is None:
                    continue  # written to the set but not read by this program
                item = {"name": name, "set": b["set"], "binding": b["binding"], "type": DESCRIPTOR_TYPES.get(b["type"], b["type"])}
                if "buffer" in b:
                    item["resource"] = index[b["buffer"]]
                    item["offset"] = b["offset"]
                    item["range"] = None if b["range"] >= (1 << 63) else b["range"]
                    if "data" in b:
                        data = (run / b["data"]).read_bytes()
                        key = hashlib.sha256(data).hexdigest()[:16]
                        constants[key] = data.hex()
                        item["constants"] = key
                elif "view" in b and b.get("image"):
                    v = views[b["view"]]
                    item["resource"] = index[b["image"]]
                    item["view_format"] = v["format"]
                    item["layout"] = b["layout"]
                bindings.append(item)
            written = {(b["set"], b["binding"]) for b in o["bindings"]}
            missing = []
            for (s, n), name in slots.items():
                if (s, n) in written:
                    continue
                fixed = immutable_sampler(o["pipeline"], s, n)
                if fixed is None:
                    missing.append(name)
                else:
                    bindings.append({"name": name, "set": s, "binding": n, "type": "immutable_sampler", "samplers": fixed})
            if missing:
                raise SystemExit("%s: interface resources without a descriptor: %s" % (entry, missing))
            frames.setdefault(str(o["frame"]), []).append({
                "label": label, "entry": entry, "shader_sha256": shader_sha[o["pipeline"]], "local_size": local,
                "required_subgroup_size": stage["required_subgroup_size"], "stage_flags": stage["flags"],
                "groups": o["groups"], "bindings": sorted(bindings, key=lambda x: (x["set"], x["binding"]))})
    uploads = []
    for o in ops:
        if o["op"] == "copy_buffer":
            data = (run / o["data"]).read_bytes()
            at = dll.find(data)
            uploads.append({"frame": o["frame"], "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                            "dll_file_offset": at if at >= 0 else None, "dst": index[o["dst"]], "dst_offset": o["dst_offset"]})
    contract = {
        "tool": "tools/fsr4fg-vk (pehost + synthetic Vulkan device; no GPU execution)",
        "dll_sha256": hashlib.sha256(dll).hexdigest(),
        "provider": selected,
        "profile": next(o for o in ops if o["op"] == "profile"),
        "memory": next(({"total": n["total"], "aliasable": n["aliasable"]} for n in notes if n.get("stage") == "memory"), None),
        "resources": resources,
        "uploads": uploads,
        # Only the image format's channels: AMD passes stale words in the others (e.g. R32G32_UINT cleared to
        # [0, 0, <last dispatch's group counts>]), which no replay should copy.
        "clears": [{"frame": o["frame"], "image": index[o["image"]],
                    "value": o["value"][:CHANNELS.get(resources[index[o["image"]]]["format"], 4)]}
                   for o in ops if o["op"] == "clear_image"],
        "copies": [{"frame": o["frame"], "src": index[o["src"]], "dst": index[o["dst"]]} for o in ops if o["op"] == "copy_image"],
        "frames": frames,
        "constants": constants,
    }
    out_path.write_text(json.dumps(contract, indent=1, sort_keys=True) + "\n")
    per = {k: len(v) for k, v in frames.items()}
    fams = sorted({p["entry"].split("_pass")[0] for v in frames.values() for p in v if p["entry"].startswith("mlfi_")})
    print(json.dumps({"provider": selected, "dispatches_per_frame": per, "families": fams,
                      "uploads": [(u["size"], u["dll_file_offset"]) for u in uploads],
                      "distinct_constant_buffers": len(constants)}))


if __name__ == "__main__":
    main()
