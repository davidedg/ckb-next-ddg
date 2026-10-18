#!/usr/bin/env python3
"""Bounded PROFILE.ZIP/qCompress codec for the K95P iCUE backup profile.

This is an offline executable specification, not daemon code.  It deliberately
accepts only the current legacy K95P contract recovered from iCUE 5.51.40.0.
Unknown fields and polymorphic types are rejected instead of being silently
discarded.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import math
from pathlib import Path
import re
import struct
import sys
from typing import Iterable, Iterator
import xml.etree.ElementTree as ET
import zlib


XML_DECLARATION = b'<?xml version="1.0" encoding="UTF-8"?>\n'
XML_SPACE = "{http://www.w3.org/XML/1998/namespace}space"
POLYMORPHIC_NEW = 0x80000000
POLYMORPHIC_STATIC = 0x40000000
ID_MASK = 0x3FFFFFFF


class ProfileCodecError(ValueError):
    """A bounded decode, XML or K95P contract check failed."""


@dataclass(frozen=True)
class Limits:
    """Resource limits for untrusted PROFILE.ZIP data.

    The largest iCUE document examined is below 80 KiB, 1,900 nodes and depth
    17.  The defaults intentionally leave ample headroom without accepting an
    allocation requested solely by the qCompress length header.
    """

    max_blob_bytes: int = 1 << 20
    max_xml_bytes: int = 4 << 20
    max_depth: int = 64
    max_nodes: int = 10_000
    max_children: int = 4_096
    max_text_chars: int = 1 << 20
    max_attributes: int = 2

    def __post_init__(self) -> None:
        if any(value <= 0 for value in vars(self).values()):
            raise ProfileCodecError("all resource limits must be positive")


@dataclass(frozen=True)
class Node:
    """Formatting-independent XML element used by the parse/build bank."""

    tag: str
    attrs: tuple[tuple[str, str], ...] = ()
    text: str = ""
    children: tuple["Node", ...] = ()

    def child(self, tag: str) -> "Node":
        matches = [child for child in self.children if child.tag == tag]
        if len(matches) != 1:
            raise ProfileCodecError(
                f"{self.tag}: expected one {tag}, found {len(matches)}"
            )
        return matches[0]


@dataclass(frozen=True)
class ProfileSummary:
    root_version: int
    nodes: int
    polymorphic_types: tuple[tuple[str, int], ...]
    actions: int
    layers: int
    macro_events: int


@dataclass(frozen=True)
class Record:
    """A cereal class with ordered semantic fields, without archive IDs."""

    kind: str
    fields: tuple[tuple[str, object], ...]

    def get(self, name: str) -> object:
        for key, value in self.fields:
            if key == name:
                return value
        raise ProfileCodecError(f"{self.kind}: missing field {name}")


@dataclass(frozen=True)
class Collection:
    items: tuple[object, ...] = ()


@dataclass(frozen=True)
class Pair:
    first: object
    second: object


@dataclass(frozen=True)
class OptionalColor:
    value: str | None


@dataclass(frozen=True)
class Pointer:
    value: Record
    identity: int | None = None  # None = unique_ptr; integer = shared identity


KNOWN_TAGS = frozenset(
    """
    ActionsProperty BrightnessIndicatorProperty LightingsProperty
    ProfileIndicatorProperty SideLightingProperty WinlockIndicatorProperty
    WinlockProperty action actionLighting actionSoundPath actions activePresetId
    angle attachedActions autoDuration base brightness button cdu cereal
    cereal_class_version clockwise color colors data delay delayBetweenCharacters
    delayMax delayMin delayMode direction disableAltF4 disableAltTab
    disableEncryption disableShiftTab disableTouchpad disableWinKey
    disabledIndicatorColor distance duration enabled enabledIndicatorColor event
    events execHint executionHints first highPriority holdingKeyEnabled
    holdingKeyOnPressInterval holdingKeyType hwActionsSupportedTypes id
    indicatorColor isAbsolute isActive isAdvancedMode isDoublesided key keyName
    keyStroke keys layer layers lighting m1Color m2Color m3Color name nullopt
    opacity option playOption polymorphic_id polymorphic_name presetsInfo profile
    profileIndicatorColor programPath ptr_wrapper random randomDelayFrom
    randomDelayTo remapGroup repeatCount repeatMode repeatOptions
    restartOnSecondExec retainOriginalKeyOutput second sniperSwitchMode speed
    startOnKeyPress startWithProfile stopAfterTimes stopOnKeyPress
    stopOnKeyRelease sub swapFKeyFunctions tailLength terminateOnSecondExec
    terminationMethod text time transitions typeLightingBehavior useRandom valid
    velocity waveSpread x y
    """.split()
)

COLLECTION_TAGS = frozenset(
    {"actions", "attachedActions", "events", "keys", "keyStroke", "layers", "transitions"}
)

BOOL_TAGS = frozenset(
    """
    autoDuration clockwise disableAltF4 disableAltTab disableEncryption
    disableShiftTab disableTouchpad disableWinKey enabled highPriority
    holdingKeyEnabled isAbsolute isActive isAdvancedMode isDoublesided nullopt
    random restartOnSecondExec retainOriginalKeyOutput startOnKeyPress
    startWithProfile stopOnKeyPress stopOnKeyRelease swapFKeyFunctions
    terminateOnSecondExec useRandom
    """.split()
)

ENUMS = {
    "action": {"PlayPause", "Stop", "PrevTrack", "NextTrack", "VolumeUp", "VolumeDown", "Mute"},
    "delayMode": {"Constant", "Random"},
    "event": {"Click", "LongPress", "ExtraLongPress", "5sLongPress"},
    "execHint": {
        "OnBoth", "OnPress", "OnRelease", "OnPressLong", "OnPressExtraLong",
        "OnPress5sLong", "WhilePressed", "Toggle",
    },
    "holdingKeyType": {"OnPress", "Toggle", "WhilePressed"},
    "layer": {
        "StandardLayer", "FunctionalLayer1", "FunctionalLayer2", "CtrlLayer",
        "AltLayer", "ShiftLayer",
    },
    "option": {"ConcreteLeds", "AllLeds"},
    "playOption": {"Standard", "PlayFromKeyCenter", "PlayOnKeyCenter"},
    "repeatMode": {"NoRepeat", "Repeat", "RepeatConstantly"},
    "sniperSwitchMode": {"OnOff", "WhilePressed"},
    "sub": {"KeyPress", "KeyRelease", "KeyStroke"},
    "terminationMethod": {
        "ExecuteUninterrupted", "AllowInterruptionsWithResume", "Interrupt"
    },
    "waveSpread": {
        "OnlyBetweenFronts", "WithAfterInnerFront", "WithBeforeOuterFront",
        "WithEveryDirection",
    },
}

INTEGER_TAGS = frozenset(
    """
    cereal_class_version delay delayBetweenCharacters delayMax delayMin direction distance
    duration holdingKeyOnPressInterval opacity randomDelayFrom randomDelayTo
    remapGroup repeatCount speed stopAfterTimes typeLightingBehavior x y
    """.split()
)

FLOAT_TAGS = frozenset({"angle", "brightness", "tailLength", "time", "velocity"})
COLOR_TAGS = frozenset(
    {
        "color", "indicatorColor", "m1Color", "m2Color", "m3Color",
    }
)

UUID_TAGS = frozenset({"actionLighting", "activePresetId"})
UUID_RE = re.compile(
    r"^\{[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\}$"
)
VALUE_RE = re.compile(r"^value(0|[1-9][0-9]*)$")
UINT_RE = re.compile(r"^(0|[1-9][0-9]*)$")
INT_RE = re.compile(r"^-?(?:0|[1-9][0-9]*)$")
FLOAT_RE = re.compile(
    r"^[+-]?(?:(?:[0-9]+(?:\.[0-9]*)?)|(?:\.[0-9]+))(?:[eE][+-]?[0-9]+)?$"
)
COLOR_RE = re.compile(r"^(rgb|rgba)\(([^()]*)\)$")


# Concrete types present in the iCUE documents examined plus the macro-event
# types iCUE 5.51.40 serializes.  The value is (class version, fields after the
# optional cereal_class_version).
TYPE_SHAPES: dict[str, tuple[int, tuple[str, ...]]] = {
    "AdvancedLightingsProperty": (200, ("base",)),
    "AdvancedLightingLayer": (200, ("base", "lighting")),
    "KeyRemapAction": (
        400,
        (
            "base", "keyName", "keyStroke", "holdingKeyEnabled", "holdingKeyType",
            "holdingKeyOnPressInterval", "programPath", "sniperSwitchMode", "remapGroup",
        ),
    ),
    "MacroAction": (201, ("base", "terminationMethod", "highPriority", "events")),
    "TextAction": (201, ("base", "text", "delayBetweenCharacters")),
    "MediaAction": (200, ("base", "action")),
    "DelayMacroActionEvent": (200, ("base", "useRandom", "delay")),
    "KeyboardMacroActionEvent": (200, ("base", "keys", "sub")),
    "MouseButtonMacroActionEvent": (200, ("base", "button", "sub")),
    "MouseMoveMacroActionEvent": (200, ("base", "isAbsolute", "x", "y")),
    "MouseScrollMacroActionEvent": (200, ("base", "x", "y")),
    "ConsumerDeviceMacroActionEvent": (200, ("base", "cdu")),
    "StaticLighting": (201, ("base", "opacity")),
    "SolidLighting": (201, ("base",)),
    "GradientLighting": (300, ("base",)),
    "WaveLighting": (201, ("base", "velocity", "tailLength", "isDoublesided", "angle")),
    "RippleLighting": (
        201, ("base", "velocity", "tailLength", "autoDuration", "waveSpread", "colors")
    ),
    "ColorPulseLighting": (300, ("base", "colors", "random", "speed")),
    "ColorShiftLighting": (300, ("base", "colors", "random", "speed")),
    "ColorWaveLighting": (300, ("value0", "speed", "random", "colors", "direction")),
    "RainLighting": (301, ("base", "colors", "random", "opacity", "speed", "direction")),
    "RainbowWaveLighting": (301, ("base", "speed", "direction")),
    "SpiralRainbowLighting": (301, ("base", "clockwise", "speed")),
    "TypeLighting": (
        401, ("base", "colors", "random", "opacity", "speed", "duration", "typeLightingBehavior")
    ),
    "VisorLighting": (301, ("base", "colors", "random", "speed", "direction")),
}

ROOT_FIELDS = (
    "cereal_class_version",
    "isAdvancedMode",
    "LightingsProperty",
    "BrightnessIndicatorProperty",
    "WinlockIndicatorProperty",
    "ProfileIndicatorProperty",
    "WinlockProperty",
    "SideLightingProperty",
    "ActionsProperty",
)

STATIC_PROPERTIES = {
    "BrightnessIndicatorProperty": (200, ("base", "indicatorColor")),
    "WinlockIndicatorProperty": (400, ("base", "enabledIndicatorColor", "disabledIndicatorColor")),
    "ProfileIndicatorProperty": (
        400, ("base", "m1Color", "m2Color", "m3Color", "profileIndicatorColor")
    ),
    "WinlockProperty": (
        300,
        (
            "base", "disableAltTab", "disableAltF4", "disableWinKey",
            "disableShiftTab", "disableEncryption", "disableTouchpad",
            "swapFKeyFunctions",
        ),
    ),
    "SideLightingProperty": (200, ("base", "enabled")),
    "ActionsProperty": (300, ("base", "hwActionsSupportedTypes")),
}


ACTION_TYPES = frozenset({"KeyRemapAction", "MacroAction", "TextAction", "MediaAction"})
# The hardware editor/serializer of this keyboard offers keyboard and delay
# events only. The other iCUE event classes are described above but not accepted.
EVENT_TYPES = frozenset({"DelayMacroActionEvent", "KeyboardMacroActionEvent"})
LIGHTING_TYPES = frozenset(name for name in TYPE_SHAPES if name.endswith("Lighting"))
POINTER_FAMILIES = {
    "action": ACTION_TYPES,
    "event": EVENT_TYPES,
    "lighting": LIGHTING_TYPES,
}

# Field descriptions are deliberately independent of the input corpus.  r:
# means a nested class, p: a cereal pointer, s: a dynamic collection; all other
# fields are checked scalar strings.  Class versions are emitted once per kind.
CLASS_SPECS: dict[str, tuple[int, tuple[tuple[str, str], ...]]] = {
    "BackupProfile": (203, tuple((tag, "scalar" if tag == "isAdvancedMode" else "p:" + (
        "AdvancedLightingsProperty" if tag == "LightingsProperty" else tag
    )) for tag in ROOT_FIELDS[1:])),
    "ProfileProperty": (200, ()),
    "ChannelPropertiesBase": (200, (("base", "r:ProfileProperty"), ("layers", "s:p:AdvancedLightingLayer"))),
    "AdvancedLightingsProperty": (200, (("base", "r:ChannelPropertiesBase"),)),
    "HardwareActionsProperty": (300, (("base", "r:ActionsBase"), ("hwActionsSupportedTypes", "u32"))),
    "ActionsBase": (303, (("base", "r:ProfileProperty"), ("actions", "s:binding"), ("presetsInfo", "p:ActionsPresets"))),
    "ActionsPresets": (400, (("activePresetId", "uuid"), ("isActive", "scalar"))),
    # An empty key: an action iCUE keeps on no key (the menu of a key, Delete, keeps the action).
    "KeyAssociation": (400, (("key", "key-or-empty"), ("layer", "scalar"), ("event", "scalar"), ("distance", "u32"))),
    "BaseAction": (202, tuple(zip(
        ("name", "id", "repeatOptions", "executionHints", "actionLighting", "actionSoundPath", "attachedActions"),
        ("scalar", "identity", "r:RepeatOptions", "r:ExecutionHints", "uuid", "scalar", "s:p:action"),
    ))),
    "RepeatOptions": (300, tuple((tag, "scalar") for tag in (
        "repeatCount", "repeatMode", "delay", "delayMode", "randomDelayFrom", "randomDelayTo"
    ))),
    "ExecutionHints": (201, tuple((tag, "scalar") for tag in (
        "terminateOnSecondExec", "restartOnSecondExec", "execHint", "retainOriginalKeyOutput"
    ))),
    "MacroActionEvent": (200, ()),
    "LightingLayer": (400, tuple(zip(
        ("id", "name", "keys", "enabled", "executionHints", "option"),
        ("identity", "scalar", "s:key", "scalar", "r:LightingExecutionHints", "scalar"),
    ))),
    "LightingExecutionHints": (200, tuple((tag, "scalar") for tag in (
        "startOnKeyPress", "startWithProfile", "stopOnKeyPress", "stopOnKeyRelease", "stopAfterTimes", "playOption"
    ))),
    "CUEAnimationBase": (300, (("name", "scalar"), ("id", "identity"), ("duration", "scalar"), ("brightness", "scalar"))),
    "CUEAnimationWithTransitions": (201, (("base", "r:CUEAnimationBase"), ("transitions", "s:r:Transition"))),
    "Transition": (200, (("time", "scalar"), ("color", "color"))),
}

for _kind, (_version, _fields) in STATIC_PROPERTIES.items():
    if _kind == "ActionsProperty":
        continue
    CLASS_SPECS[_kind] = (_version, tuple(
        (tag, "r:ProfileProperty" if tag == "base" else (
            "optional-color" if tag in {"enabledIndicatorColor", "disabledIndicatorColor", "profileIndicatorColor"}
            else "color" if tag in COLOR_TAGS else "scalar"
        )) for tag in _fields
    ))

for _kind, (_version, _fields) in TYPE_SHAPES.items():
    if _kind in CLASS_SPECS:
        continue
    _specs = []
    for _tag in _fields:
        if _tag == "base" or (_kind == "ColorWaveLighting" and _tag == "value0"):
            if _kind in ACTION_TYPES:
                _spec = "r:BaseAction"
            elif _kind in EVENT_TYPES:
                _spec = "r:MacroActionEvent"
            elif _kind == "AdvancedLightingLayer":
                _spec = "r:LightingLayer"
            elif _kind in {"StaticLighting", "SolidLighting", "GradientLighting", "WaveLighting", "RippleLighting"}:
                _spec = "r:CUEAnimationWithTransitions"
            else:
                _spec = "r:CUEAnimationBase"
        else:
            _spec = {
                "lighting": "p:lighting", "events": "s:p:event", "keys": "s:key",
                "keyStroke": "s:key", "colors": "colors", "keyName": "key-or-empty",
                "button": "u32", "cdu": "u32",
            }.get(_tag, "scalar")
        _specs.append((_tag, _spec))
    CLASS_SPECS[_kind] = (_version, tuple(_specs))


def _expected_pointer_types(spec: str) -> frozenset[str]:
    return POINTER_FAMILIES.get(spec, frozenset({spec}))


def validate_solid(effect: Record) -> None:
    """A Solid block owns one colour at both endpoints; preserve its original representation."""
    if effect.kind != 'SolidLighting':
        return
    base = effect.get('base')
    if not isinstance(base, Record) or not isinstance(base.get('transitions'), Collection):
        raise _error('SolidLighting: invalid transitions')
    points = base.get('transitions').items
    if len(points) % 2:
        raise _error('SolidLighting: unpaired block endpoint')
    def rgba(point):
        if not isinstance(point, Record) or not isinstance(point.get('color'), str):
            raise _error('SolidLighting: invalid colour')
        _validate_scalar(Node('color', text=point.get('color')), '')
        match = COLOR_RE.fullmatch(point.get('color'))
        if not match:
            raise _error('SolidLighting: invalid colour')
        values = tuple(float(value) for value in match.group(2).split())
        return values if len(values) == 4 else values + (1.,)
    for i in range(0, len(points), 2):
        if rgba(points[i]) != rgba(points[i + 1]):
            raise _error('SolidLighting: a block has different endpoint colours')


def validate_lighting(layer: Record) -> None:
    """Reject values outside the known iCUE editor domains, including muted layers."""
    effect = layer.get('lighting')
    if isinstance(effect, Pointer):
        effect = effect.value
    if not isinstance(effect, Record):
        raise _error('invalid lighting effect')
    validate_solid(effect)
    def bounded(value, low, high, step=1):
        try:
            number = float(value)
        except (ValueError, TypeError, OverflowError):
            return False
        return math.isfinite(number) and low <= number <= high and number % step == 0
    hints = layer.get('base').get('executionHints')
    if effect.kind in {'StaticLighting', 'SolidLighting', 'GradientLighting', 'WaveLighting', 'RippleLighting'}:
        previous = 0.
        for point in effect.get('base').get('transitions').items:
            time = float(point.get('time'))
            if not math.isfinite(time) or not previous <= time <= 1:
                raise _error('transition time outside iCUE limits')
            previous = time
            _validate_scalar(Node('color', text=point.get('color')), '')
    if not bounded(hints.get('stopAfterTimes'), -1, 99):
        raise _error('repeat count outside iCUE limits')
    custom = {'SolidLighting', 'GradientLighting', 'WaveLighting', 'RippleLighting'}
    if effect.kind in custom and not bounded(effect.get('base').get('base').get('duration'), 100, 99900, 100):
        raise _error('duration outside iCUE limits (0.1..99.9 s, steps of 0.1 s)')
    if effect.kind in {'WaveLighting', 'RippleLighting'}:
        if any(not bounded(effect.get(field), 1, 999) for field in ('tailLength', 'velocity')):
            raise _error('tail or velocity outside iCUE limits (0.1..99.9, steps of 0.1)')
    if effect.kind == 'WaveLighting' and not bounded(effect.get('angle'), 0, 359):
        raise _error('angle outside iCUE limits (integer 0..359)')
    if effect.kind in {'StaticLighting', 'RainLighting', 'TypeLighting'} and not bounded(effect.get('opacity'), 0, 255):
        raise _error('opacity outside iCUE limits')
    if effect.kind.endswith('Lighting') and effect.kind not in custom | {'StaticLighting'}:
        if not bounded(effect.get('speed'), 1, 3):
            raise _error('speed outside iCUE limits')
    if effect.kind in {'ColorWaveLighting', 'RainbowWaveLighting'} and not bounded(effect.get('direction'), 1, 4):
        raise _error('direction outside iCUE limits')
    if effect.kind == 'TypeLighting' and (not bounded(effect.get('typeLightingBehavior'), 0, 1) or
                                           not bounded(effect.get('duration'), 1, 3)):
        raise _error('type-lighting mode or duration outside iCUE limits')


class ModelReader:
    """Resolve archive-local type/object IDs into checked semantic records."""

    def __init__(self) -> None:
        self.versions: dict[str, int] = {}
        self.types: dict[int, str] = {}
        self.objects: dict[int, Pointer] = {}
        self.pending_objects: set[int] = set()
        self.identities: set[str] = set()
        self.next_object = 1

    def record(self, node: Node, kind: str) -> Record:
        if kind == "ActionsProperty":
            kind = "HardwareActionsProperty"
        version, specs = CLASS_SPECS[kind]
        children = list(node.children)
        if kind not in self.versions:
            if not children or children[0].tag != "cereal_class_version":
                raise _error(f"{kind}: first occurrence lacks cereal_class_version")
            got = _uint(children.pop(0).text, kind + "/cereal_class_version")
            if got != version:
                raise _error(f"{kind}: version {got}, expected {version}")
            self.versions[kind] = got
        elif children and children[0].tag == "cereal_class_version":
            raise _error(f"{kind}: repeated cereal_class_version")
        if kind == "DelayMacroActionEvent" and any(
            c.tag == "useRandom" and c.text == "true" for c in children
        ):
            specs = (("base", "r:MacroActionEvent"), ("useRandom", "scalar"), ("delayMin", "scalar"), ("delayMax", "scalar"))
        expected = tuple(tag for tag, _ in specs)
        if tuple(c.tag for c in children) != expected:
            raise _error(f"{kind}: fields {_fields(node)!r}, expected {expected!r}")
        result = Record(kind, tuple(
            (tag, self.value(child, spec))
            for child, (tag, spec) in zip(children, specs)
        ))
        validate_solid(result)
        if kind == 'AdvancedLightingLayer':
            validate_lighting(result)
        return result

    def value(self, node: Node, spec: str) -> object:
        if spec.startswith("r:"):
            return self.record(node, spec[2:])
        if spec.startswith("p:"):
            return self.pointer(node, spec[2:])
        if spec.startswith("s:"):
            if node.attrs != (("size", "dynamic"),):
                raise _error(f"{node.tag}: missing dynamic collection attribute")
            return Collection(tuple(self.value(child, spec[2:]) for child in node.children))
        if spec in {"binding", "colors"}:
            if _fields(node) != ("first", "second"):
                raise _error(f"{node.tag}: expected ordered first/second pair")
            first_spec, second_spec = ("p:action", "r:KeyAssociation") if spec == "binding" else ("u32", "u32")
            return Pair(self.value(node.children[0], first_spec), self.value(node.children[1], second_spec))
        if spec == "optional-color":
            fields = _fields(node)
            if fields == ("nullopt",) and node.children[0].text == "true":
                return OptionalColor(None)
            if fields == ("nullopt", "data") and node.children[0].text == "false":
                return OptionalColor(str(self.value(node.children[1], "color")))
            raise _error(f"{node.tag}: malformed optional color")
        if node.children:
            raise _error(f"{node.tag}: scalar contains child elements")
        if spec == "u32":
            _uint(node.text, node.tag)
        elif spec in {"uuid", "identity"}:
            if not UUID_RE.fullmatch(node.text):
                raise _error(f"{node.tag}: malformed UUID")
            if spec == "identity":
                if node.text == "{00000000-0000-0000-0000-000000000000}" or node.text in self.identities:
                    raise _error(f"{node.tag}: null or duplicate object UUID {node.text}")
                self.identities.add(node.text)
        elif spec == "color":
            # Validate even color-bearing fields named data, first or second.
            _validate_scalar(Node("color", text=node.text), "")
        elif spec in {"key", "key-or-empty"}:
            if not node.text and spec != "key-or-empty":
                raise _error(f"{node.tag}: empty key name")
            if node.text and not re.fullmatch(r"[A-Za-z0-9_]+", node.text):
                raise _error(f"{node.tag}: malformed key name {node.text!r}")
        return node.text

    def pointer(self, node: Node, expected: str) -> Pointer:
        fields = _fields(node)
        if fields not in {("polymorphic_id", "ptr_wrapper"), ("polymorphic_id", "polymorphic_name", "ptr_wrapper")}:
            raise _error(f"{node.tag}: invalid cereal pointer structure")
        raw = _uint(node.child("polymorphic_id").text, node.tag)
        static = raw == POLYMORPHIC_STATIC
        allowed = _expected_pointer_types(expected)
        if static:
            if len(allowed) != 1 or len(fields) != 2:
                raise _error(f"{node.tag}: invalid static pointer for {expected}")
            kind = expected
        elif raw & POLYMORPHIC_STATIC:
            raise _error(f"{node.tag}: invalid type marker {raw:#x}")
        elif raw & POLYMORPHIC_NEW:
            type_id = raw & ID_MASK
            if type_id != len(self.types) + 1 or len(fields) != 3:
                raise _error(f"{node.tag}: invalid new polymorphic ID/name")
            kind = node.child("polymorphic_name").text
            if kind in self.types.values():
                raise _error(f"{node.tag}: duplicate polymorphic registration {kind}")
            self.types[type_id] = kind
        else:
            if raw not in self.types or len(fields) != 2:
                raise _error(f"{node.tag}: unknown or malformed polymorphic reference {raw}")
            kind = self.types[raw]
        if kind not in allowed:
            raise _error(f"{node.tag}: {kind!r} is not an allowed {expected} type")
        wrapper = node.child("ptr_wrapper")
        fields = _fields(wrapper)
        unique = expected in set(STATIC_PROPERTIES) | {"AdvancedLightingsProperty"}
        if unique:
            if fields != ("valid", "data") or wrapper.children[0].text != "1":
                raise _error(f"{node.tag}: expected non-null unique pointer")
            return Pointer(self.record(wrapper.children[1], kind))
        if fields not in {("id",), ("id", "data")}:
            raise _error(f"{node.tag}: expected shared pointer")
        raw = _uint(wrapper.children[0].text, node.tag + "/ptr_wrapper/id")
        if raw & POLYMORPHIC_STATIC:
            raise _error(f"{node.tag}: invalid shared pointer marker")
        if raw & POLYMORPHIC_NEW:
            identity = raw & ID_MASK
            if identity != self.next_object or len(fields) != 2:
                raise _error(f"{node.tag}: invalid shared pointer definition")
            self.next_object += 1
            self.pending_objects.add(identity)
            value = self.record(wrapper.children[1], kind)
            pointer = Pointer(value, identity)
            self.pending_objects.remove(identity)
            self.objects[identity] = pointer
            return pointer
        if raw in self.pending_objects:
            raise _error(f"{node.tag}: cyclic shared pointer is outside the K95P contract")
        if raw not in self.objects or len(fields) != 1:
            raise _error(f"{node.tag}: undefined/redefined shared pointer {raw}")
        result = self.objects[raw]
        if result.value.kind != kind:
            raise _error(f"{node.tag}: shared pointer type mismatch")
        return result


class ModelWriter:
    """Create fresh class versions and cereal registries from the model."""

    def __init__(self) -> None:
        self.versions: set[str] = set()
        self.types: dict[str, int] = {}
        self.objects: dict[int, tuple[int, Record]] = {}

    def record(self, record: Record, tag: str) -> Node:
        if record.kind not in CLASS_SPECS:
            raise _error(f"unknown model class {record.kind!r}")
        children: list[Node] = []
        if record.kind not in self.versions:
            self.versions.add(record.kind)
            children.append(Node("cereal_class_version", text=str(CLASS_SPECS[record.kind][0])))
        children.extend(self.value(value, name) for name, value in record.fields)
        return Node(tag, children=tuple(children))

    def value(self, value: object, tag: str) -> Node:
        if isinstance(value, Record):
            return self.record(value, tag)
        if isinstance(value, Collection):
            return Node(tag, (("size", "dynamic"),), children=tuple(
                self.value(item, f"value{index}") for index, item in enumerate(value.items)
            ))
        if isinstance(value, Pair):
            return Node(tag, children=(self.value(value.first, "first"), self.value(value.second, "second")))
        if isinstance(value, OptionalColor):
            children = [Node("nullopt", text="true" if value.value is None else "false")]
            if value.value is not None:
                children.append(Node("data", text=value.value))
            return Node(tag, children=tuple(children))
        if isinstance(value, Pointer):
            kind = value.value.kind
            # The public root tag names the static HardwareActionsProperty.
            static = kind not in TYPE_SHAPES
            children = []
            if static:
                children.append(Node("polymorphic_id", text=str(POLYMORPHIC_STATIC)))
            elif kind in self.types:
                children.append(Node("polymorphic_id", text=str(self.types[kind])))
            else:
                identity = len(self.types) + 1
                self.types[kind] = identity
                children.extend((Node("polymorphic_id", text=str(POLYMORPHIC_NEW | identity)), Node("polymorphic_name", text=kind)))
            if value.identity is None:
                wrapper = (Node("valid", text="1"), self.record(value.value, "data"))
            elif value.identity in self.objects:
                identity, previous = self.objects[value.identity]
                if previous != value.value:
                    raise _error(f"shared model identity {value.identity} describes different objects")
                wrapper = (Node("id", text=str(identity)),)
            else:
                identity = len(self.objects) + 1
                self.objects[value.identity] = (identity, value.value)
                wrapper = (Node("id", text=str(POLYMORPHIC_NEW | identity)), self.record(value.value, "data"))
            children.append(Node("ptr_wrapper", children=wrapper))
            return Node(tag, children=tuple(children))
        if not isinstance(value, str):
            raise _error(f"{tag}: invalid model value {type(value).__name__}")
        attrs = (("xml:space", "preserve"),) if tag == "text" and value and value.isspace() else ()
        return Node(tag, attrs, text=value)


def read_model(root: Node) -> Record:
    return ModelReader().record(root.child("profile"), "BackupProfile")


def make_record(kind: str, **fields: object) -> Record:
    """Build a class in the field order of iCUE; build_document validates values."""
    if kind not in CLASS_SPECS:
        raise _error(f"unknown model class {kind!r}")
    expected = tuple(tag for tag, _ in CLASS_SPECS[kind][1])
    if kind == "DelayMacroActionEvent" and fields.get("useRandom") == "true":
        expected = ("base", "useRandom", "delayMin", "delayMax")
    if set(fields) != set(expected):
        raise _error(f"{kind}: model fields differ: missing={set(expected) - set(fields)}, extra={set(fields) - set(expected)}")
    return Record(kind, tuple((name, fields[name]) for name in expected))


def build_document(model: Record, limits: Limits = Limits()) -> Node:
    if model.kind != "BackupProfile":
        raise _error("model root must be BackupProfile")
    # Bound a caller-built model before recursively expanding it as XML.
    stack: list[tuple[object, int]] = [(model, 1)]
    objects = text_chars = 0
    while stack:
        value, depth = stack.pop()
        objects += 1
        if depth > limits.max_depth or objects > limits.max_nodes:
            raise _error("semantic model exceeds depth/object limit")
        if isinstance(value, str):
            text_chars += len(value)
            if len(value) > limits.max_text_chars or text_chars > limits.max_xml_bytes:
                raise _error("semantic model exceeds text limit")
        elif isinstance(value, Record):
            if len(value.fields) > limits.max_children:
                raise _error("semantic model exceeds field limit")
            stack.extend((item, depth + 1) for _, item in value.fields)
        elif isinstance(value, Collection):
            if len(value.items) > limits.max_children:
                raise _error("semantic model exceeds collection limit")
            stack.extend((item, depth + 1) for item in value.items)
        elif isinstance(value, Pointer):
            stack.append((value.value, depth + 1))
        elif isinstance(value, Pair):
            stack.extend(((value.first, depth + 1), (value.second, depth + 1)))
        elif isinstance(value, OptionalColor):
            if value.value is not None:
                stack.append((value.value, depth + 1))
        else:
            raise _error(f"invalid semantic model value {type(value).__name__}")
    root = Node("cereal", children=(ModelWriter().record(model, "profile"),))
    validate_document(root, limits)
    return root


def _error(message: str) -> ProfileCodecError:
    return ProfileCodecError(message)


def decode_qcompress(blob: bytes, limits: Limits = Limits()) -> bytes:
    """Decode a Qt qCompress byte array without trusting its size header."""
    if len(blob) > limits.max_blob_bytes:
        raise _error(f"qCompress blob too large: {len(blob)} > {limits.max_blob_bytes}")
    if len(blob) < 4:
        raise _error("qCompress blob shorter than its four-byte length header")
    declared = struct.unpack(">I", blob[:4])[0]
    if declared > limits.max_xml_bytes:
        raise _error(
            f"qCompress declared size too large: {declared} > {limits.max_xml_bytes}"
        )
    compressed = blob[4:]
    if not compressed:
        raise _error("qCompress blob has no zlib stream")

    inflater = zlib.decompressobj()
    try:
        payload = inflater.decompress(compressed, declared + 1)
    except zlib.error as exc:
        raise _error(f"invalid qCompress zlib stream: {exc}") from exc
    if len(payload) > declared or inflater.unconsumed_tail:
        raise _error("qCompress output exceeds the declared size")
    if not inflater.eof:
        raise _error("truncated qCompress zlib stream")
    if inflater.unused_data:
        raise _error("trailing bytes after qCompress zlib stream")
    try:
        payload += inflater.flush()
    except zlib.error as exc:
        raise _error(f"invalid qCompress zlib trailer: {exc}") from exc
    if len(payload) != declared:
        raise _error(f"qCompress length mismatch: header {declared}, decoded {len(payload)}")
    return payload


def encode_qcompress(xml: bytes, limits: Limits = Limits()) -> bytes:
    """Encode the Qt format used by iCUE: u32 BE length plus zlib level 9."""
    if len(xml) > limits.max_xml_bytes:
        raise _error(f"XML too large: {len(xml)} > {limits.max_xml_bytes}")
    blob = struct.pack(">I", len(xml)) + zlib.compress(xml, level=9)
    if len(blob) > limits.max_blob_bytes:
        raise _error(f"encoded blob too large: {len(blob)} > {limits.max_blob_bytes}")
    return blob


def strip_xml_annotation(source: bytes) -> bytes:
    """Remove the one-line "declared=N got=N" annotation some extractors put before the XML."""
    start = source.find(b"<?xml")
    if start < 0:
        raise _error("XML declaration not found")
    prefix = source[:start]
    if prefix:
        annotation = re.fullmatch(rb"declared=([0-9]{1,10}) got=([0-9]{1,10})\n", prefix)
        if annotation is None or annotation.group(1) != annotation.group(2):
            raise _error("unsupported bytes before XML declaration")
    return source[start:]


def _normal_attr(name: str) -> str:
    return "xml:space" if name == XML_SPACE else name


def _node_from_element(element: ET.Element, limits: Limits) -> Node:
    attrs = tuple((_normal_attr(name), value) for name, value in element.attrib.items())
    if len(element):
        if element.text and not element.text.isspace():
            raise _error(f"{element.tag}: mixed character content is not supported")
        children = tuple(_node_from_element(child, limits) for child in element)
        for child in element:
            if child.tail and not child.tail.isspace():
                raise _error(f"{element.tag}: non-whitespace text after {child.tag}")
        return Node(element.tag, attrs, "", children)
    text = element.text or ""
    if len(text) > limits.max_text_chars:
        raise _error(
            f"{element.tag}: text too long: {len(text)} > {limits.max_text_chars}"
        )
    return Node(element.tag, attrs, text, ())


def parse_xml(source: bytes, limits: Limits = Limits()) -> Node:
    """Parse UTF-8 XML with limits, then validate the current K95P contract."""
    if len(source) > limits.max_xml_bytes:
        raise _error(f"XML too large: {len(source)} > {limits.max_xml_bytes}")
    if not source.startswith(XML_DECLARATION):
        raise _error("non-canonical or missing UTF-8 XML declaration")
    upper = source.upper()
    if b"<!DOCTYPE" in upper or b"<!ENTITY" in upper:
        raise _error("DTD and entity declarations are forbidden")
    body = source[len(XML_DECLARATION):]
    if b"<!--" in body or b"<![CDATA[" in upper or b"<?" in body:
        raise _error("comments, CDATA and processing instructions are outside the contract")
    try:
        source.decode("utf-8", errors="strict")
    except UnicodeDecodeError as exc:
        raise _error(f"XML is not valid UTF-8: {exc}") from exc

    parser = ET.XMLPullParser(events=("start", "end"))
    depth = nodes = 0
    root_element: ET.Element | None = None
    try:
        for offset in range(0, len(source), 16_384):
            parser.feed(source[offset : offset + 16_384])
            for event, element in parser.read_events():
                if event == "start":
                    depth += 1
                    nodes += 1
                    if depth > limits.max_depth:
                        raise _error(f"XML depth exceeds {limits.max_depth}")
                    if nodes > limits.max_nodes:
                        raise _error(f"XML nodes exceed {limits.max_nodes}")
                    if len(element.attrib) > limits.max_attributes:
                        raise _error(
                            f"{element.tag}: attributes exceed {limits.max_attributes}"
                        )
                else:
                    if len(element) > limits.max_children:
                        raise _error(
                            f"{element.tag}: children exceed {limits.max_children}"
                        )
                    depth -= 1
                    if depth == 0:
                        root_element = element
        parser.close()
    except ET.ParseError as exc:
        raise _error(f"malformed XML: {exc}") from exc
    if depth != 0:
        raise _error("unbalanced XML depth")
    if root_element is None:
        raise _error("empty XML document")
    root = _node_from_element(root_element, limits)
    validate_document(root, limits)
    return root


def _escape_text(value: str) -> str:
    # QXmlStreamWriter escapes quotation marks in character data too.
    return (
        value.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
        .replace("\r", "&#13;")
    )


def _escape_attr(value: str) -> str:
    return _escape_text(value).replace("\r", "&#13;").replace("\n", "&#10;")


def _emit_node(node: Node, depth: int, lines: list[str]) -> None:
    indent = "\t" * depth
    attrs = "".join(f' {name}="{_escape_attr(value)}"' for name, value in node.attrs)
    if not node.children:
        if not node.text and (node.tag == "base" or dict(node.attrs).get("size") == "dynamic"):
            lines.append(f"{indent}<{node.tag}{attrs}/>")
        else:
            text = _escape_text(node.text)
            lines.append(f"{indent}<{node.tag}{attrs}>{text}</{node.tag}>")
        return
    lines.append(f"{indent}<{node.tag}{attrs}>")
    for child in node.children:
        _emit_node(child, depth + 1, lines)
    lines.append(f"{indent}</{node.tag}>")


def emit_xml(root: Node, limits: Limits = Limits()) -> bytes:
    """Emit the canonical tab-indented UTF-8 form used by current iCUE."""
    validate_document(root, limits)
    lines: list[str] = []
    _emit_node(root, 0, lines)
    try:
        payload = XML_DECLARATION + ("\n".join(lines) + "\n").encode("utf-8")
    except UnicodeEncodeError as exc:
        raise _error(f"model contains non-UTF-8 text: {exc}") from exc
    if len(payload) > limits.max_xml_bytes:
        raise _error(f"emitted XML too large: {len(payload)} > {limits.max_xml_bytes}")
    return payload


def _path(parent: str, node: Node) -> str:
    return f"{parent}/{node.tag}" if parent else node.tag


def _uint(text: str, where: str) -> int:
    if len(text) > 10 or not UINT_RE.fullmatch(text):
        raise _error(f"{where}: expected canonical unsigned integer, got {text!r}")
    value = int(text)
    if value > 0xFFFFFFFF:
        raise _error(f"{where}: integer exceeds u32")
    return value


def _validate_scalar(node: Node, parent: str) -> None:
    where = _path(parent, node)
    parent_tag = parent.rsplit("/", 1)[-1]
    if node.tag in BOOL_TAGS and node.text not in {"true", "false"}:
        raise _error(f"{where}: expected true or false")
    if node.tag in ENUMS and node.text not in ENUMS[node.tag]:
        raise _error(f"{where}: unsupported enum value {node.text!r}")
    if node.tag in INTEGER_TAGS:
        if len(node.text) > 11 or not INT_RE.fullmatch(node.text):
            raise _error(f"{where}: expected canonical integer, got {node.text!r}")
        value = int(node.text)
        if node.tag in {"x", "y", "stopAfterTimes"}:
            if not -(1 << 31) <= value < (1 << 31):
                raise _error(f"{where}: integer outside i32")
        elif not 0 <= value <= 0xFFFFFFFF:
            raise _error(f"{where}: integer outside u32")
        ranges = {"opacity": (0, 255), "speed": (0, 3), "direction": (0, 12), "typeLightingBehavior": (0, 1)}
        if node.tag in ranges and not ranges[node.tag][0] <= value <= ranges[node.tag][1]:
            raise _error(f"{where}: integer outside supported range {ranges[node.tag]}")
    if node.tag in FLOAT_TAGS:
        if not FLOAT_RE.fullmatch(node.text) or not math.isfinite(float(node.text)):
            raise _error(f"{where}: expected finite floating-point value")
    if node.tag in UUID_TAGS and node.text and not UUID_RE.fullmatch(node.text):
        raise _error(f"{where}: malformed UUID {node.text!r}")
    if node.tag == "valid" and node.text not in {"0", "1"}:
        raise _error(f"{where}: valid must be 0 or 1")
    if node.tag in {"polymorphic_id"} or (node.tag == "id" and parent_tag == "ptr_wrapper"):
        _uint(node.text, where)
    if node.tag in COLOR_TAGS:
        match = COLOR_RE.fullmatch(node.text)
        if match is None:
            raise _error(f"{where}: unsupported QColor form {node.text!r}")
        expected = 3 if match.group(1) == "rgb" else 4
        parts = match.group(2).split()
        if len(parts) != expected:
            raise _error(f"{where}: QColor needs {expected} components")
        for part in parts:
            if not FLOAT_RE.fullmatch(part):
                raise _error(f"{where}: malformed QColor component {part!r}")
            value = float(part)
            if not math.isfinite(value) or not 0.0 <= value <= 1.0:
                raise _error(f"{where}: QColor component outside 0..1")


def _validate_tree(node: Node, parent: str = "") -> None:
    where = _path(parent, node)
    if node.tag not in KNOWN_TAGS and not VALUE_RE.fullmatch(node.tag):
        raise _error(f"{where}: unknown element")
    attrs = dict(node.attrs)
    if len(attrs) != len(node.attrs):
        raise _error(f"{where}: duplicate attributes")
    allowed: dict[str, str] = {}
    if node.tag in COLLECTION_TAGS:
        allowed["size"] = "dynamic"
    if node.tag == "text":
        allowed["xml:space"] = "preserve"
    for name, value in node.attrs:
        if allowed.get(name) != value:
            raise _error(f"{where}: unsupported attribute {name}={value!r}")
    if node.children and node.text:
        raise _error(f"{where}: mixed content")
    if attrs.get("size") == "dynamic":
        expected = tuple(f"value{index}" for index in range(len(node.children)))
        actual = tuple(child.tag for child in node.children)
        if actual != expected:
            raise _error(f"{where}: dynamic collection is not value0..valueN-1")
    if not node.children:
        _validate_scalar(node, parent)
    for child in node.children:
        _validate_tree(child, where)


def _fields(node: Node) -> tuple[str, ...]:
    return tuple(child.tag for child in node.children)




def validate_document(root: Node, limits: Limits = Limits()) -> ProfileSummary:
    """Validate the structural K95P contract and cereal identity spaces."""
    stack = [(root, 1)]
    nodes = total_text = 0
    while stack:
        node, depth = stack.pop()
        nodes += 1
        if depth > limits.max_depth or nodes > limits.max_nodes:
            raise _error("model exceeds depth/node limit")
        if len(node.children) > limits.max_children or len(node.attrs) > limits.max_attributes:
            raise _error(f"{node.tag}: model exceeds children/attribute limit")
        if len(node.text) > limits.max_text_chars:
            raise _error(f"{node.tag}: model exceeds text limit")
        total_text += len(node.text)
        if total_text > limits.max_xml_bytes:
            raise _error("model exceeds total text limit")
        # XML 1.0 characters, checked for programmatically built models too.
        for value in (node.text, *(v for _, v in node.attrs)):
            if any(not (c in "\t\r\n" or 0x20 <= ord(c) <= 0xD7FF or 0xE000 <= ord(c) <= 0xFFFD or 0x10000 <= ord(c) <= 0x10FFFF) for c in value):
                raise _error(f"{node.tag}: invalid XML 1.0 character")
        stack.extend((child, depth + 1) for child in node.children)
    _validate_tree(root)
    if root.tag != "cereal" or root.attrs or root.text or len(root.children) != 1:
        raise _error("document root must be a single <cereal><profile> hierarchy")
    profile = root.children[0]
    if profile.tag != "profile" or profile.attrs or _fields(profile) != ROOT_FIELDS:
        raise _error(f"profile fields differ from version-203 contract: {_fields(profile)!r}")
    root_version = _uint(profile.children[0].text, "cereal/profile/cereal_class_version")
    if root_version != 203:
        raise _error(f"unsupported profile version {root_version}; expected 203")
    if profile.children[1].text != "true":
        raise _error("cereal/profile/isAdvancedMode must be true")

    # This checks every nested class, version, field order and pointer family,
    # including common bases which the outer pointer-only check cannot cover.
    read_model(root)
    counts: dict[str, int] = {}
    registry: dict[int, str] = {}
    for node in walk(root):
        if "polymorphic_id" not in _fields(node):
            continue
        raw = int(node.child("polymorphic_id").text)
        if raw == POLYMORPHIC_STATIC:
            continue
        if raw & POLYMORPHIC_NEW:
            registry[raw & ID_MASK] = node.child("polymorphic_name").text
        name = registry[raw & ID_MASK]
        counts[name] = counts.get(name, 0) + 1
    types = tuple(sorted(counts.items()))
    type_counts = dict(types)
    if type_counts.get("AdvancedLightingsProperty") != 1:
        raise _error("profile must contain exactly one AdvancedLightingsProperty")

    def collection_count(tag: str) -> int:
        return sum(len(node.children) for node in walk(root) if node.tag == tag)

    return ProfileSummary(
        root_version=root_version,
        nodes=sum(1 for _ in walk(root)),
        polymorphic_types=types,
        actions=collection_count("actions"),
        layers=collection_count("layers"),
        macro_events=collection_count("events"),
    )


def walk(root: Node) -> Iterator[Node]:
    yield root
    for child in root.children:
        yield from walk(child)


def load_document(path: Path, limits: Limits = Limits()) -> tuple[Node, bytes]:
    source = read_limited(path, limits.max_blob_bytes if path.suffix.lower() == ".bin" else limits.max_xml_bytes)
    if path.suffix.lower() == ".bin":
        xml = decode_qcompress(source, limits)
    else:
        xml = strip_xml_annotation(source)
    return parse_xml(xml, limits), xml


def read_limited(path: Path, maximum: int) -> bytes:
    with path.open("rb") as source:
        payload = source.read(maximum + 1)
    if len(payload) > maximum:
        raise _error(f"{path}: input file exceeds {maximum} bytes")
    return payload


def corpus_xml_files(root: Path) -> list[Path]:
    return sorted(path for path in root.rglob("*.xml") if "PROFILE.ZIP" in path.name)


def _command_validate(paths: Iterable[Path]) -> int:
    failed = 0
    for path in paths:
        try:
            document, _ = load_document(path)
            summary = validate_document(document)
            types = ",".join(f"{name}:{count}" for name, count in summary.polymorphic_types)
            print(
                f"OK {path}: v{summary.root_version} nodes={summary.nodes} "
                f"actions={summary.actions} layers={summary.layers} "
                f"events={summary.macro_events} types={types}"
            )
        except (OSError, ProfileCodecError) as exc:
            failed += 1
            print(f"FAIL {path}: {exc}", file=sys.stderr)
    return 1 if failed else 0


def _command_roundtrip(root_path: Path) -> int:
    files = corpus_xml_files(root_path)
    exact = semantic = failed = 0
    for path in files:
        try:
            document, source = load_document(path)
            model = read_model(document)
            rebuilt = build_document(model)
            emitted = emit_xml(rebuilt)
            reparsed = parse_xml(emitted)
            if read_model(reparsed) != model or rebuilt != document:
                raise _error("emitted document changed the parsed model")
            semantic += 1
            exact += emitted == source
        except (OSError, ProfileCodecError) as exc:
            failed += 1
            print(f"FAIL {path}: {exc}", file=sys.stderr)
    print(
        f"PROFILE.XML documents={len(files)} semantic-roundtrips={semantic} "
        f"byte-identical={exact} normalized={semantic - exact} failures={failed}"
    )
    return 1 if failed else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    validate = subparsers.add_parser("validate", help="validate XML or qCompress .bin files")
    validate.add_argument("paths", nargs="+", type=Path)
    roundtrip = subparsers.add_parser("roundtrip", help="parse/emit all PROFILE.ZIP XML below ROOT")
    roundtrip.add_argument("root", type=Path)
    decode = subparsers.add_parser("decode", help="decode and validate qCompress input")
    decode.add_argument("input", type=Path)
    decode.add_argument("output", type=Path)
    encode = subparsers.add_parser("encode", help="validate XML and encode qCompress level 9")
    encode.add_argument("input", type=Path)
    encode.add_argument("output", type=Path)
    args = parser.parse_args()

    try:
        if args.command == "validate":
            return _command_validate(args.paths)
        if args.command == "roundtrip":
            return _command_roundtrip(args.root)
        if args.command == "decode":
            xml = decode_qcompress(read_limited(args.input, Limits().max_blob_bytes))
            parse_xml(xml)
            args.output.write_bytes(xml)
            return 0
        if args.command == "encode":
            xml = strip_xml_annotation(read_limited(args.input, Limits().max_xml_bytes))
            parse_xml(xml)
            args.output.write_bytes(encode_qcompress(xml))
            return 0
    except (OSError, ProfileCodecError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    raise AssertionError(args.command)


if __name__ == "__main__":
    sys.exit(main())
