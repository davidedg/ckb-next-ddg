#!/usr/bin/env python3
"""icue-local-to-xml.py - the PROFILE.ZIP document of a hardware profile, made from iCUE's local copy of that profile.

    python3 icue-local-to-xml.py LOCAL.cueprofiledata -o PROFILE.xml [--meta META.json]

iCUE keeps a local copy of every hardware slot it has read or written (%APPDATA%\\Corsair\\CUE5\\hw_profiles\\{GUID}.cueprofiledata,
cereal root profile 506, the properties in ::Proxy wrappers keyed by channel or mode) and stores the same classes in the slot as
a BackupProfile 203 document, the XML of PROFILE.ZIP. iCUE offers no way to write a local copy back to a slot. The document made
here is the one iCUE would have put in the ZIP; `k95p-hwslot-tool zip --xml PROFILE.xml` compiles the files of the slot from
it.

Taken from the local copy, each in exactly the form given or the copy is refused (nothing is guessed):
  - LightingsProperty: the AdvancedLightingsProperty of channel 0 of the one AggregatedLightingsProperty entry, the keyboard's
    StandardRgb/Default lighting; the AdvancedLightingsProperty proxies of the copy hold no layers (iCUE keeps them empty),
  - BrightnessIndicatorProperty, WinlockIndicatorProperty: the entry false of their proxies (keys true and false), the one
    iCUE's ZIP holds where the two differ; for BrightnessIndicatorProperty, which no copy shows different, they must be equal,
  - ProfileIndicatorProperty, WinlockProperty, SideLightingProperty: as they are,
  - ActionsProperty: the HardwareActionsProperty of the one entry of its proxy, its actions in the reverse order (the copy lists
    them from the last of the ZIP to the first); the software ActionsProperty must have no action,
  - isAdvancedMode: true, the value of every PROFILE.ZIP iCUE 5.51.40 wrote for this keyboard (the copy has no such field).
The other properties of the copy (gestures, joystick, screen modes, ...) are not part of a hardware slot, as in iCUE's ZIP.

Cereal resolves polymorphic names and shared objects in the order of the whole document, and the copy can define a class or an
object in a property the ZIP does not hold: both are resolved over the whole copy here, and written again with the numbering
of the new document (profile_xml_codec.ModelWriter). The classes and their versions are those of profile_xml_codec; a class
version that differs from it refuses the copy. --meta writes the identity of the copy as JSON: name, id (the GUID of the
slot), cookie, blobCookie, dpiCookie, slotIndex, deviceId.
Exit status: 0 the document was written, 2 the copy was refused or could not be read.
"""

from __future__ import annotations

import argparse
import json
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

# the codec lives next to this script (python3 -I leaves the script's directory out of sys.path)
sys.path.insert(0, str(Path(__file__).resolve().parent))
import profile_xml_codec as codec  # noqa: E402

LOCAL_ROOT_VERSION = 506
MAX_BYTES = 8 * 1024 * 1024
LIGHTING_KEY = {"deviceId": "", "hidCaps": "StandardRgb", "deviceLightingType": "Default"}
# The K95 RGB Platinum: USB 1b1c:1b2d
MODEL_ID = {"usbVid": "6940", "usbPid": "6957"}


class Refused(Exception):
    pass


def refuse(why: str) -> None:
    raise Refused(why)


class LocalReader(codec.ModelReader):
    """profile_xml_codec's reader, with the type and object registries of the whole local document: a pointer may name a
    class first registered, or an object first defined, in a part of the copy that is not converted."""

    def __init__(self, root: ET.Element):
        super().__init__()
        self.global_types: dict[int, str] = {}
        self.defs: dict[int, ET.Element] = {}
        self.node_of: dict[int, codec.Node] = {}
        for el in root.iter():
            pid = el.find("polymorphic_id")
            if pid is not None and pid.text and int(pid.text) & codec.POLYMORPHIC_NEW and not int(pid.text) & codec.POLYMORPHIC_STATIC:
                n = int(pid.text) & codec.ID_MASK
                if n != len(self.global_types) + 1:
                    refuse(f"polymorphic id {n} out of order")
                name = el.findtext("polymorphic_name") or ""
                if name in self.global_types.values():
                    refuse(f"class {name} registered twice")
                self.global_types[n] = name
            if el.tag == "ptr_wrapper":
                oid = el.find("id")
                if oid is not None and oid.text and int(oid.text) & codec.POLYMORPHIC_NEW and el.find("data") is not None:
                    n = int(oid.text) & codec.ID_MASK
                    if n in self.defs:
                        refuse(f"shared object {n} defined twice")
                    self.defs[n] = el
        self.seen_versions: dict[str, int] = {}
        self.unversioned: set[str] = set()

    def record(self, node: codec.Node, kind: str) -> codec.Record:
        if kind == "ActionsProperty":
            kind = "HardwareActionsProperty"
        if kind not in codec.CLASS_SPECS:
            refuse(f"class {kind} is not one of the hardware profile")
        version = codec.CLASS_SPECS[kind][0]
        children = list(node.children)
        if children and children[0].tag == "cereal_class_version":
            got = int(children.pop(0).text)
            if got != version:
                refuse(f"{kind}: version {got}, the level has {version}")
            self.seen_versions[kind] = got
        elif kind not in self.seen_versions:
            self.unversioned.add(kind)
        # the parent reader wants the version exactly once per kind: give it the node as it expects it
        if kind in self.versions:
            fixed = codec.Node(node.tag, node.attrs, node.text, tuple(children))
        else:
            fixed = codec.Node(node.tag, node.attrs, node.text,
                               (codec.Node("cereal_class_version", text=str(version)),) + tuple(children))
        return super().record(fixed, kind)

    def pointer(self, node: codec.Node, expected: str) -> codec.Pointer:
        fields = tuple(c.tag for c in node.children)
        if fields not in {("polymorphic_id", "ptr_wrapper"), ("polymorphic_id", "polymorphic_name", "ptr_wrapper")}:
            refuse(f"{node.tag}: not a cereal pointer")
        raw = int(node.child("polymorphic_id").text)
        allowed = codec._expected_pointer_types(expected)
        if raw == codec.POLYMORPHIC_STATIC:
            if len(allowed) != 1:
                refuse(f"{node.tag}: a static pointer for {expected}")
            kind = expected
        elif raw & codec.POLYMORPHIC_STATIC:
            refuse(f"{node.tag}: type marker {raw:#x}")
        else:
            kind = self.global_types.get(raw & codec.ID_MASK)
            if kind is None:
                refuse(f"{node.tag}: unknown polymorphic id {raw}")
            if bool(raw & codec.POLYMORPHIC_NEW) != (len(fields) == 3):
                refuse(f"{node.tag}: polymorphic name and id disagree")
            if len(fields) == 3 and node.child("polymorphic_name").text != kind:
                refuse(f"{node.tag}: polymorphic name and id disagree")
        if kind not in allowed:
            refuse(f"{node.tag}: {kind} is not an allowed {expected}")
        wrapper = node.child("ptr_wrapper")
        wfields = tuple(c.tag for c in wrapper.children)
        if expected in set(codec.STATIC_PROPERTIES) | {"AdvancedLightingsProperty"}:
            if wfields != ("valid", "data") or wrapper.children[0].text != "1":
                refuse(f"{node.tag}: expected a non-null unique pointer")
            return codec.Pointer(self.record(wrapper.children[1], kind))
        if wfields not in {("id",), ("id", "data")}:
            refuse(f"{node.tag}: expected a shared pointer")
        oid = int(wrapper.children[0].text)
        if oid & codec.POLYMORPHIC_STATIC:
            refuse(f"{node.tag}: shared pointer marker {oid:#x}")
        identity = oid & codec.ID_MASK
        if bool(oid & codec.POLYMORPHIC_NEW) != (len(wfields) == 2):
            refuse(f"{node.tag}: shared pointer {identity} malformed")
        if identity in self.objects:
            result = self.objects[identity]
        else:
            if identity in self.pending_objects:
                refuse(f"{node.tag}: cyclic shared pointer")
            if identity not in self.defs:
                refuse(f"{node.tag}: shared object {identity} is not defined")
            data = codec._node_from_element(self.defs[identity].find("data"), codec.Limits())
            self.pending_objects.add(identity)
            result = codec.Pointer(self.record(data, kind), identity)
            self.pending_objects.remove(identity)
            self.objects[identity] = result
        if result.value.kind != kind:
            refuse(f"{node.tag}: shared object {identity} is a {result.value.kind}, here a {kind}")
        return result


def node(el: ET.Element) -> codec.Node:
    return codec._node_from_element(el, codec.Limits())


def classes(root: ET.Element) -> dict[str, list[ET.Element]]:
    """The device properties of the copy by class: group (Profile, Keyboard) / class name."""
    names: dict[int, str] = {}
    for el in root.iter():
        pid = el.find("polymorphic_id")
        if pid is not None and pid.text and int(pid.text) & codec.POLYMORPHIC_NEW and not int(pid.text) & codec.POLYMORPHIC_STATIC:
            names[int(pid.text) & codec.ID_MASK] = el.findtext("polymorphic_name") or ""
    out: dict[str, list[ET.Element]] = {}
    props = root.find("profile/properties")
    if props is None:
        refuse("no profile/properties")
    for group in props:
        gname = group.findtext("key")
        inner = group.find("value/properties")
        if inner is None:
            refuse(f"group {gname}: no properties")
        for v in inner:
            pid = int(v.findtext("polymorphic_id") or "0")
            cname = names.get(pid & codec.ID_MASK, "?") if not pid & codec.POLYMORPHIC_STATIC else "(static)"
            out.setdefault(f"{gname}/{cname}", []).append(v)
    return out


def one(cls: dict[str, list[ET.Element]], name: str) -> ET.Element:
    found = cls.get(name, [])
    if len(found) != 1:
        refuse(f"{name}: {len(found)} in the copy, expected one")
    return found[0]


def entries(prop: ET.Element) -> list[ET.Element]:
    """The entries (key, value) of a ::Proxy property."""
    data = prop.find("ptr_wrapper/data")
    p = data.find("properties") if data is not None else None
    if p is None:
        refuse("a proxy without properties")
    return list(p)


def key_fields(entry: ET.Element) -> dict[str, str]:
    k = entry.find("key")
    out = {}
    for c in k:
        if c.tag == "cereal_class_version":
            continue
        if len(c):
            out[c.tag] = {g.tag: (g.text or "") for g in c if g.tag != "cereal_class_version"}
        else:
            out[c.tag] = c.text or ""
    return out


def lighting(reader: LocalReader, cls) -> codec.Pointer:
    for name in ("Keyboard/AdvancedLightingsProperty::Proxy",):
        for e in entries(one(cls, name)):
            if any(len(l) for l in e.iter("layers")):
                refuse(f"{name} [{e.findtext('key')}] holds layers: not the form iCUE keeps")
    agg = entries(one(cls, "Keyboard/AggregatedLightingsProperty::Proxy"))
    if len(agg) != 1:
        refuse(f"AggregatedLightingsProperty: {len(agg)} entries, expected one")
    k = key_fields(agg[0])
    model = k.pop("modelId", None)
    if k != LIGHTING_KEY or model not in (MODEL_ID, ""):
        refuse(f"AggregatedLightingsProperty: entry {k} {model}, expected the keyboard's StandardRgb/Default")
    data = agg[0].find("value/ptr_wrapper/data")
    if agg[0].findtext("value/polymorphic_id") != str(codec.POLYMORPHIC_STATIC) or data is None:
        refuse("AggregatedLightingsProperty: entry not a static pointer")
    tags = [c.tag for c in data if c.tag != "cereal_class_version"]
    if tags != ["base", "channelProperties"] or len(data.find("base")):
        refuse(f"AggregatedLightingsProperty: fields {tags}")
    channels = list(data.find("channelProperties"))
    if len(channels) != 1 or channels[0].findtext("key") != "0":
        refuse(f"AggregatedLightingsProperty: {len(channels)} channels, expected channel 0 only")
    values = list(channels[0].find("value"))
    if len(values) != 1:
        refuse(f"AggregatedLightingsProperty: channel 0 has {len(values)} lightings, expected one")
    return reader.pointer(node(values[0]), "AdvancedLightingsProperty")


def indicator(reader: LocalReader, cls, name: str, proved: bool) -> codec.Pointer:
    """The entry false of the proxy: iCUE's ZIP holds that one (a real pair, local copy and the ZIP of the same save, where the
    entries of WinlockIndicatorProperty differ). For a property no such pair proves, the two entries must be equal."""
    ents = entries(one(cls, f"Keyboard/{name}::Proxy"))
    if sorted(e.findtext("key") for e in ents) != ["false", "true"]:
        refuse(f"{name}: entries {[e.findtext('key') for e in ents]}, expected true and false")
    got = {e.findtext("key"): reader.pointer(node(e.find("value")), name) for e in ents}
    if not proved and got["true"] != got["false"]:
        refuse(f"{name}: the entries true and false differ, and which one the slot holds is not proved")
    return got["false"]


def direct(reader: LocalReader, cls, name: str) -> codec.Pointer:
    el = one(cls, f"Keyboard/{name}")
    # the property is registered by name in the copy, a static pointer in the ZIP
    wrapper = el.find("ptr_wrapper")
    static = codec.Node(el.tag, children=(codec.Node("polymorphic_id", text=str(codec.POLYMORPHIC_STATIC)), node(wrapper)))
    return reader.pointer(static, name)


def actions(reader: LocalReader, cls) -> codec.Pointer:
    soft = entries(one(cls, "Keyboard/ActionsProperty::Proxy"))
    if any(len(a) for e in soft for a in e.iter("actions")):
        refuse("the software ActionsProperty has actions: not a hardware profile as iCUE keeps one")
    hw = entries(one(cls, "Keyboard/HardwareActionsProperty::Proxy"))
    if len(hw) != 1 or key_fields(hw[0]) != {"identifier": "", "types": "0"}:
        refuse(f"HardwareActionsProperty: entries {[key_fields(e) for e in hw]}, expected one, identifier empty and types 0")
    prop = reader.pointer(node(hw[0].find("value")), "ActionsProperty").value
    # the copy lists the actions in the reverse order of the ZIP (every pair of copy and ZIP compared)
    base = prop.get("base")
    items = base.get("actions").items
    base = replace(base, actions=codec.Collection(tuple(reversed(items))))
    return codec.Pointer(replace(prop, base=base))


def replace(record: codec.Record, **fields) -> codec.Record:
    return codec.Record(record.kind, tuple((k, fields.get(k, v)) for k, v in record.fields))


def meta(root: ET.Element, cls) -> dict:
    el = one(cls, "Profile/HardwareMetaProperty").find("ptr_wrapper/data")
    out = {"name": root.findtext("profile/name"), "id": root.findtext("profile/id")}
    for tag in ("cookie", "blobCookie", "dpiCookie", "slotIndex", "deviceId"):
        v = el.findtext(tag)
        if v is None:
            refuse(f"HardwareMetaProperty: no {tag}")
        out[tag] = int(v) if tag != "deviceId" else v
    if not codec.UUID_RE.fullmatch(out["id"] or ""):
        refuse(f"profile id {out['id']!r} is not a GUID")
    return out


def versions_elsewhere(reader: LocalReader, cls) -> None:
    """Cereal writes the version of a class once, at its first occurrence in the document, and in the copy that can be in a
    part the ZIP does not hold: those parts are read too, for their versions only (and they must hold nothing): the base of
    HardwareMetaProperty (ProfileProperty), the empty AdvancedLightingsProperty proxies, the software ActionsProperty, and the
    base of the first touchpad gesture of GesturesProperty (a property of no keyboard slot, which in copies of iCUE 5.48 holds
    the first BaseAction of the document: only that base is read, for the versions of BaseAction, RepeatOptions and
    ExecutionHints)."""
    base = one(cls, "Profile/HardwareMetaProperty").find("ptr_wrapper/data/base")
    if base is None:
        refuse("HardwareMetaProperty: no base")
    reader.record(node(base), "ProfileProperty")
    for e in entries(one(cls, "Keyboard/AdvancedLightingsProperty::Proxy")):
        p = reader.pointer(node(e.find("value")), "AdvancedLightingsProperty")
        if p.value.get("base").get("layers").items:
            refuse("AdvancedLightingsProperty proxy with layers")
    for e in entries(one(cls, "Keyboard/ActionsProperty::Proxy")):
        data = e.find("value/ptr_wrapper/data")
        if e.findtext("value/polymorphic_id") != str(codec.POLYMORPHIC_STATIC) or data is None:
            refuse("the software ActionsProperty: entry not a static pointer")
        if reader.record(node(data), "ActionsBase").get("actions").items:
            refuse("the software ActionsProperty has actions")
    gestures = cls.get("Keyboard/GesturesProperty", [])
    if len(gestures) > 1:
        refuse(f"Keyboard/GesturesProperty: {len(gestures)} in the copy, expected at most one")
    for g in gestures:
        base = g.find("ptr_wrapper/data/gestures/value0/value/second/ptr_wrapper/data/base")
        if base is not None and base.find("cereal_class_version") is not None:
            reader.record(node(base), "BaseAction")


def convert(data: bytes) -> tuple[bytes, dict, set[str]]:
    if len(data) > MAX_BYTES:
        refuse("the copy is too large")
    upper = data.upper()
    if b"<!DOCTYPE" in upper or b"<!ENTITY" in upper:
        refuse("DTD and entity declarations are forbidden")
    root = ET.fromstring(data)
    v = root.findtext("profile/cereal_class_version")
    if v != str(LOCAL_ROOT_VERSION):
        refuse(f"root profile version {v}, expected {LOCAL_ROOT_VERSION}")
    cls = classes(root)
    reader = LocalReader(root)
    fields = {
        "isAdvancedMode": "true",
        "LightingsProperty": lighting(reader, cls),
        "BrightnessIndicatorProperty": indicator(reader, cls, "BrightnessIndicatorProperty", False),
        "WinlockIndicatorProperty": indicator(reader, cls, "WinlockIndicatorProperty", True),
        "ProfileIndicatorProperty": direct(reader, cls, "ProfileIndicatorProperty"),
        "WinlockProperty": direct(reader, cls, "WinlockProperty"),
        "SideLightingProperty": direct(reader, cls, "SideLightingProperty"),
        "ActionsProperty": actions(reader, cls),
    }
    model = codec.make_record("BackupProfile", **fields)
    xml = codec.emit_xml(codec.build_document(model))
    # the document made reads back as the same model with the reader of the ZIPs
    if codec.read_model(codec.parse_xml(xml)) != codec.read_model(codec.build_document(model)):
        refuse("the document made does not read back as its model")
    versions_elsewhere(reader, cls)
    unversioned = reader.unversioned - set(reader.seen_versions)
    if unversioned:
        refuse(f"the version of {', '.join(sorted(unversioned))} is nowhere in the copy")
    return xml, meta(root, cls), unversioned


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("local", type=Path)
    ap.add_argument("-o", "--output", type=Path, required=True)
    ap.add_argument("--meta", type=Path)
    a = ap.parse_args()
    try:
        xml, m, _ = convert(a.local.read_bytes())
    except (OSError, ET.ParseError, codec.ProfileCodecError, Refused, ValueError) as e:
        print(f"icue-local-to-xml: {a.local}: {e}", file=sys.stderr)
        return 2
    a.output.write_bytes(xml)
    if a.meta:
        a.meta.write_text(json.dumps(m, indent=2) + "\n")
    print(f"icue-local-to-xml: {m['name']!r} {m['id']} cookie {m['cookie']} slotIndex {m['slotIndex']}: {a.output} {len(xml)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
