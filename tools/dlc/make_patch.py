"""Writes Azura's GearGame script patch for RAAM's Shadow from the player's own disc files.

RAAM's Shadow's packages import a few classes, properties and functions of the GearGame script
package that only the game's Title Update has. The disc build reads script patches from the update
volume (GearGame/Patches/ScriptPatch_GearGame.bin), so Azura serves one that declares them. Its
content is Azura's own, kept to what the DLC needs to load and play:
  - classes the DLC levels place or extend, as script subclasses of the disc classes they extend,
    with the properties the levels and the DLC's script use, and default objects whose components
    are copies of their parents';
  - those properties and functions on disc classes (functions with empty bodies: the DLC overrides
    them or only needs them to exist).
Every index and name comes from the disc's GearGame package, read from the game folder given on the
command line; nothing is taken from the Title Update. See docs/raams-shadow-dlc.md.

The launcher uses the runtime's C++ version (runtime/src/dlc/raams_shadow_patch.cpp, `azura
--make_dlc_patch`), which writes the same bytes; this one is for trying changes, which must then be
made there too.

Usage: make_patch.py <game folder> <output folder> [--uncompressed <cache file>] [--only <names>]
Writes <output>/GearGame/Patches/ScriptPatch_GearGame.bin and its _us size file, and
<output>/dlc_patch.json with the SHA-256 of the GearGame.xxx it was made for.
"""

import hashlib
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # ue3 and lzx, also under python -I
import ue3  # noqa: E402
from ue3 import Patch, Writer, parse_class  # noqa: E402
import bytecode as bc  # noqa: E402

# --- Content -------------------------------------------------------------------------------------

# Property kinds: (type, extra). Object and component properties name their class by path.
# Flags: CPF_Edit (0x1); component properties also CPF_ExportObject (0x8), CPF_Component (0x80000)
# and CPF_EditInline (0x4000000), as the disc's own component properties have them.
EDIT = 0x1
TRANSIENT = 0x2000
COMPONENT_FLAGS = 0x0000000004080009

# New classes: name, parent (GearGame class name or 'Engine.Class'), properties, and optionally
# {'within': its ClassWithin (a GearGame class), 'functions': [(name, the disc function whose flags
# it takes, parameters, body)] as in FUNCTIONS, 'states': [(name, script)], 'defaults': [tagged
# property]}, the functions in their declaration order. A state's script is a function given the
# Builder and returning its code (see bytecode.py), which starts at its label 'Begin'. The default
# object's tagged properties are ('float', name, value), ('bool', name, value) or
# ('struct', name, struct, [tagged property]); in a struct also ('enum', name, enum, value),
# ('name', name, value) and ('structs', name, [[tagged property]]), an array of structs.
CLASSES = [
    # The drop of a Locust from a grappling hook: its parent's fall, started with the burst of an
    # emergence (its animations) and a downward speed, as the Title Update has it.
    ('GSM_GrapplingHook_DynamicClimbDrop', 'GSM_BaseVariableFall', [
        ('FallStartSpeedZ', 'FloatProperty', EDIT, None),
    ], {
        'functions': [
            ('SpecialMoveStarted', 'GSM_BaseVariableFall.SpecialMoveStarted', 'GSM_BaseVariableFall.SpecialMoveStarted',
             ('script', lambda b, f: grappling_drop_started(b, f))),
        ],
        'defaults': [
            ('float', 'FallStartSpeedZ', -400.0),
            ('struct', 'BS_Jump', 'BodyStance', [('structs', 'Stance', [[
                ('enum', 'Slot', 'EBodyStance', 'BS_FullBody'), ('name', 'AnimName', 'Emerge_Burst_fall')]])]),
            ('struct', 'BS_Fall', 'BodyStance', [('structs', 'Stance', [[
                ('enum', 'Slot', 'EBodyStance', 'BS_FullBody'), ('name', 'AnimName', 'Emerge_Burst_fall')]])]),
            ('struct', 'BS_Land', 'BodyStance', [('structs', 'Stance', [[
                ('enum', 'Slot', 'EBodyStance', 'BS_FullBody'), ('name', 'AnimName', 'Emerge_Burst_land')]])]),
            ('bool', 'bLockPawnRotation', True),
        ],
    }),
    ('GearPawn_LocustEliteBoomerFlailBase', 'GearPawn_LocustBoomerFlailBase', []),
    ('Turret_SatelliteHODBase', 'Turret_TroikaBase', []),
    ('GearPawn_SatelliteHODCamera_Base', 'GearPawn', [
        ('SatelliteTurret', 'ObjectProperty', EDIT, 'GearGame.Turret_SatelliteHODBase'),
        ('HODPostProcessEffect', 'ObjectProperty', EDIT, 'Engine.PostProcessEffect'),
        # Azura's own: where GearGRI.SHODTargets keeps the camera's targets (see MEMBERS).
        ('SHODTargetStore', 'ArrayProperty', TRANSIENT, ('ObjectProperty', 0, 'Engine.Actor')),
    ]),
    ('PawnBlockingVolume', 'Engine.DynamicBlockingVolume', [
        ('AllowedClasses', 'ArrayProperty', EDIT, ('ClassProperty', EDIT, 'Core.Class', 'Engine.Pawn')),
    ]),
    ('SeqEvent_BerserkerVulnerable', 'Engine.SequenceEvent', []),
    ('ThumperSpikeBase', 'Engine.Actor', [
        ('LightEnvironment', 'ComponentProperty', COMPONENT_FLAGS, 'Engine.DynamicLightEnvironmentComponent'),
        ('MeshComp', 'ComponentProperty', COMPONENT_FLAGS, 'Engine.StaticMeshComponent'),
    ]),
    # The DLC's Brumak (Caravan) attacks its scripted targets, the convoy's vehicles, every so often:
    # the AI command its utility-based AI picks for that, as the Title Update has it.
    ('AICmd_Brumak_Action_AttackScriptedTargets', 'AICmd_Base_UtilitySelectedCmd', [], {
        'within': 'GearAI_Brumak',
        'functions': [
            ('EvaluateActivation', 'AICmd_Base_UtilitySelectedCmd.EvaluateActivation',
             'AICmd_Base_UtilitySelectedCmd.EvaluateActivation', ('script', lambda b, f: brumak_evaluate_activation(b, f))),
            ('Pushed', 'AICommand.Popped', [], ('script', lambda b, f: brumak_pushed(b))),
            ('Popped', 'AICommand.Popped', [], ('script', lambda b, f: brumak_popped(b))),
        ],
        'states': [('AttackTruck', lambda b: brumak_attack_truck(b))],
    }),
    ('GearDLCCampaignActData', 'GearCampaignActData', []),
]

# Properties on disc classes: class, name, type, flags, extra. The runtime gives each its place in
# the class (runtime/src/dlc/class_fixups.h).
MEMBERS = [
    ('GearPawn_LocustGeneralRaamBase', 'bShieldDispersed', 'BoolProperty', 0, None),
    ('Vehicle_Reaver_Base', 'TaccomMesh', 'ComponentProperty', COMPONENT_FLAGS, 'Engine.StaticMeshComponent'),
    # The satellite camera's enemies (GearPawn_SatelliteHODCamera.AddEnemyLocators, the only script
    # that uses it, through GearGRI.AddSHODTarget). GearGRI has no room for it, so the runtime points
    # it at the camera's SHODTargetStore while the camera runs that function; its inner property is
    # an int, of the same size as an object reference, so that GearGRI's garbage collection token
    # stream gets nothing for it (the store's own property is the one the collector follows).
    ('GearGRI', 'SHODTargets', 'ArrayProperty', 0, ('IntProperty', 0)),
]

# Functions on disc classes: class, name, the disc function whose flags they take, parameters, body.
# Parameters are copies of disc parameters ('Class.Function.Param'), or ('Name', 'IntProperty'),
# ('Name', 'NameProperty') or ('Name', 'ObjectProperty', 'Package.Class') for a new one, with a
# fourth item, 0, for a local variable; in the order of the function's field chain: the script
# compiler chains parameters from the last declared to the first, then the locals. Bodies are
# 'empty' (return;), ('super', 'Class.Function'): return the parent's function with the same
# parameters, ('copy', {disc path: replacement}): the bytecode of the disc function the flags come
# from, with each reference it names replaced (by a GearGame path, or by the name of one of the new
# function's parameters), or ('script', function): the bytecode that function returns (see
# bytecode.py), given the Builder and the new function's fields by name.
FUNCTIONS = [
    ('GearPawn', 'NonFinalActivateRoadieRunComet', 'GearPawn.ActivateRoadieRunComet', [], 'empty'),
    # if (SHODTargets.Find(Target) == INDEX_NONE) SHODTargets.AddItem(Target);, as AddTaccomTurret
    # does with its own array.
    ('GearGRI', 'AddSHODTarget', 'GearGRI.AddTaccomTurret', [('Target', 'ObjectProperty', 'Engine.Actor')],
     ('copy', {'GearGRI.TaccomTurrets': 'GearGame.GearGRI.SHODTargets', 'GearGRI.AddTaccomTurret.Turret': 'Target'})),
    # Myrrah's transmissions (SeqAct_MyrrahTalk) and the satellite camera turn a post process effect
    # of their own on and off for each player. Reliable client functions, as the disc's other
    # GearPC.Client* functions.
    ('GearPC', 'EnablePostProcessEffects', 'GearPC.ClientColorFade',
     [('EffectToEnable', 'ObjectProperty', 'Engine.MaterialEffect'),
      ('Chain', 'ObjectProperty', 'Engine.PostProcessChain'),
      ('ChainName', 'NameProperty'),
      ('LP', 'ObjectProperty', 'Engine.LocalPlayer', 0),
      ('MIC', 'ObjectProperty', 'Engine.MaterialInstanceConstant', 0)],
     ('script', lambda b, f: enable_post_process_effects(b, f))),
    ('GearPC', 'DisablePostProcessEffects', 'GearPC.ClientColorFade',
     [('EffectToDisable', 'ObjectProperty', 'Engine.MaterialEffect'),
      ('ChainName', 'NameProperty'),
      ('I', 'IntProperty', None, 0),
      ('LP', 'ObjectProperty', 'Engine.LocalPlayer', 0)],
     ('script', lambda b, f: disable_post_process_effects(b, f))),
]


def enable_post_process_effects(b, f):
    """LP = LocalPlayer(Player);
    if (LP != None) {
        LP.InsertPostProcessingChain(Chain, -1, true);
        EffectToEnable = MaterialEffect(LP.PlayerPostProcess.FindPostProcessEffect(ChainName));
        if (EffectToEnable != None) {
            MIC = new class'MaterialInstanceConstant';
            MIC.SetParent(EffectToEnable.Material);
            EffectToEnable.Material = MIC;
            EffectToEnable.bShowInGame = true;
        }
    }"""
    r = b.ref
    lp, effect = bc.local(f['LP']), bc.local(f['EffectToEnable'])
    material = r('Engine.MaterialEffect.Material')
    show = r('Engine.PostProcessEffect.bShowInGame')
    post_process = r('Engine.LocalPlayer.PlayerPostProcess')
    find = r('Engine.PostProcessChain.FindPostProcessEffect')
    return bc.seq(
        bc.let(lp, bc.cast(r('Engine.LocalPlayer'), bc.instance(r('Engine.PlayerController.Player')))),
        bc.jump_if_not('end', bc.native(bc.NOT_EQUAL_OBJECT_OBJECT, lp, bc.none())),
        bc.context(lp, bc.call(b.name('InsertPostProcessingChain'), bc.local(f['Chain']), bc.int_const(-1), bc.true()),
                   r('Engine.LocalPlayer.InsertPostProcessingChain.ReturnValue')),
        bc.let(effect, bc.cast(r('Engine.MaterialEffect'), bc.context(
            bc.context(lp, bc.instance(post_process), post_process),
            bc.call_final(find, bc.local(f['ChainName'])), r('Engine.PostProcessChain.FindPostProcessEffect.ReturnValue')))),
        bc.jump_if_not('end', bc.native(bc.NOT_EQUAL_OBJECT_OBJECT, effect, bc.none())),
        bc.let(bc.local(f['MIC']), bc.new(r('Engine.MaterialInstanceConstant'))),
        bc.context(bc.local(f['MIC']), bc.call(b.name('SetParent'), bc.context(effect, bc.instance(material), material))),
        bc.let(bc.context(effect, bc.instance(material), material), bc.local(f['MIC'])),
        bc.let_bool(bc.context(effect, bc.bool_variable(bc.instance(show)), show), bc.true()),
        bc.label('end'), bc.ret(), bc.end())


def disable_post_process_effects(b, f):
    """if (EffectToDisable != None) EffectToDisable.bShowInGame = false;
    LP = LocalPlayer(Player);
    if (LP != None) {
        LP.ClearPostProcessSettingsOverride();
        for (I = 1; I < LP.PlayerPostProcessChains.Length; I++)
            if (LP.PlayerPostProcessChains[I].FindPostProcessEffect(ChainName) != None)
                LP.RemovePostProcessingChain(I--);
    }"""
    r = b.ref
    lp, effect, i = bc.local(f['LP']), bc.local(f['EffectToDisable']), bc.local(f['I'])
    show = r('Engine.PostProcessEffect.bShowInGame')
    chains = r('Engine.LocalPlayer.PlayerPostProcessChains')
    return bc.seq(
        bc.jump_if_not('player', bc.native(bc.NOT_EQUAL_OBJECT_OBJECT, effect, bc.none())),
        bc.let_bool(bc.context(effect, bc.bool_variable(bc.instance(show)), show), bc.false()),
        bc.label('player'),
        bc.let(lp, bc.cast(r('Engine.LocalPlayer'), bc.instance(r('Engine.PlayerController.Player')))),
        bc.jump_if_not('end', bc.native(bc.NOT_EQUAL_OBJECT_OBJECT, lp, bc.none())),
        bc.context(lp, bc.call(b.name('ClearPostProcessSettingsOverride'), bc.empty_parm())),
        bc.let(i, bc.int_one()),
        bc.label('loop'),
        bc.jump_if_not('end', bc.native(bc.LESS_INT_INT, i, bc.array_length(bc.context(lp, bc.instance(chains), chains)))),
        bc.jump_if_not('next', bc.native(bc.NOT_EQUAL_OBJECT_OBJECT, bc.context(
            bc.array_element(i, bc.context(lp, bc.instance(chains), chains)),
            bc.call_final(r('Engine.PostProcessChain.FindPostProcessEffect'), bc.local(f['ChainName'])),
            r('Engine.PostProcessChain.FindPostProcessEffect.ReturnValue')), bc.none())),
        bc.context(lp, bc.call(b.name('RemovePostProcessingChain'), i), r('Engine.LocalPlayer.RemovePostProcessingChain.ReturnValue')),
        bc.native(bc.SUBTRACT_SUBTRACT_INT, i),
        bc.label('next'),
        bc.native(bc.ADD_ADD_INT, i),
        bc.jump('loop'),
        bc.label('end'), bc.ret(), bc.end())

def grappling_drop_started(b, f):
    """Super.SpecialMoveStarted(bForced, PrevMove); PlayFall(0.0); PawnOwner.Velocity.Z = FallStartSpeedZ;"""
    velocity = b.ref('Engine.Actor.Velocity')
    return bc.seq(
        bc.call_final(b.ref('GearGame.GSM_BaseVariableFall.SpecialMoveStarted'),
                      bc.bool_variable(bc.local(f['bForced'])), bc.local(f['PrevMove'])),
        bc.call(b.name('PlayFall'), bc.float_const(0.0)),
        bc.let(bc.struct_member(b.ref('Core.Object.Vector.Z'), b.ref('Core.Object.Vector'),
                                bc.context(bc.instance(b.ref('GearGame.GearSpecialMove.PawnOwner')), bc.instance(velocity), velocity)),
               bc.instance(b.ref('GearGame.GSM_GrapplingHook_DynamicClimbDrop.FallStartSpeedZ'))),
        bc.ret(), bc.end())


# Seconds between the Brumak's attacks on its scripted targets: the DLC's GearAI_Brumak_DLC sets it
# (AttackScriptedTargetInterval), in a property the Title Update adds to GearAI_Brumak, which the
# patch cannot add (a native class with a subclass the disc build loads with it).
BRUMAK_ATTACK_INTERVAL = 13.0


def brumak_evaluate_activation(b, f):
    """static function float EvaluateActivation(GearAI AI, UtilBasedAICmdMetaDataPacket ActivationData):
    return 1.2 when the AI has targets, the Brumak's main gun is not destroyed, and the interval has
    passed since the AI was made and since this command last ran; -1 otherwise."""
    r = b.ref
    ai = bc.local(f['AI'])
    world, now, created = r('Engine.Actor.WorldInfo'), r('Engine.WorldInfo.TimeSeconds'), r('Engine.Actor.CreationTime')
    targets, pawn = r('GearGame.GearAI.TargetList'), r('Engine.Controller.Pawn')
    last = r('GearGame.UtilBasedAICmdMetaDataPacket.LastActivationTime')
    gun = r('GearGame.GearPawn_LocustBrumakBase.IsMainGunDestroyed')
    since = lambda start: bc.native(bc.SUBTRACT_FLOAT_FLOAT, bc.context(bc.context(ai, bc.instance(world), world), bc.instance(now), now), start)
    interval = bc.float_const(BRUMAK_ATTACK_INTERVAL)
    return bc.seq(
        bc.jump_if_not('no', bc.and_also(bc.and_also(bc.and_also(
            bc.native(bc.GREATER_INT_INT, bc.array_length(bc.context(ai, bc.instance(targets), targets)), bc.int_one()),
            bc.native(bc.NOT_PRE_BOOL, bc.context(
                bc.cast(r('GearGame.GearPawn_LocustBrumakBase'), bc.context(ai, bc.instance(pawn), pawn)),
                bc.call_final(gun), r('GearGame.GearPawn_LocustBrumakBase.IsMainGunDestroyed.ReturnValue')))),
            bc.native(bc.GREATER_FLOAT_FLOAT, since(bc.context(ai, bc.instance(created), created)), interval)),
            bc.native(bc.GREATER_FLOAT_FLOAT, since(bc.context(bc.local(f['ActivationData']), bc.instance(last), last)), interval))),
        bc.ret(bc.float_const(1.2)),
        bc.label('no'), bc.ret(bc.float_const(-1.0)),
        bc.return_nothing(f['ReturnValue']), bc.end())


def brumak_pushed(b):
    """Super.Pushed(); GotoState('AttackTruck');"""
    return bc.seq(bc.call_final(b.ref('GameFramework.GameAICommand.Pushed')),
                  bc.native(bc.GOTO_STATE, bc.name_const(b.name('AttackTruck')), bc.empty_parm(), bc.empty_parm(), bc.empty_parm()),
                  bc.ret(), bc.end())


def brumak_outer(b, prop):
    """Outer.<prop> (the command's outer is its AI, a GearAI_Brumak)."""
    outer, member = b.ref('Core.Object.Outer'), b.ref(prop)
    return bc.context(bc.instance(outer), bc.instance(member), member)


def brumak_popped(b):
    """Super.Popped(); each gun's AI selects its enemy again; Outer.Focus = Outer.FireTarget;"""
    select = lambda gun: bc.context(brumak_outer(b, gun), bc.call(b.name('SelectEnemy')),
                                    b.ref('GearGame.GearAI_Brumak_Slave.SelectEnemy.ReturnValue'))
    return bc.seq(bc.call_final(b.ref('GearGame.AICommand.Popped')),
                  select('GearGame.GearAI_Brumak.LeftGunAI'), select('GearGame.GearAI_Brumak.RightGunAI'),
                  select('GearGame.GearAI_Brumak.DriverAI'),
                  bc.let(brumak_outer(b, 'Engine.Controller.Focus'), brumak_outer(b, 'GearGame.GearAI.FireTarget')),
                  bc.ret(), bc.end())


def brumak_attack_truck(b):
    """Begin: if (Outer.SelectScriptedTarget()) { Outer.DriverAI.SetScriptedFireTarget(Outer.FireTarget,
    false, 3.0); Outer.DriverAI.FireRockets(Outer); } Outer.PopCommand(self); Stop;"""
    outer = bc.instance(b.ref('Core.Object.Outer'))
    driver, fire_target = brumak_outer(b, 'GearGame.GearAI_Brumak.DriverAI'), brumak_outer(b, 'GearGame.GearAI.FireTarget')
    return bc.seq(
        bc.label('Begin'),
        bc.jump_if_not('pop', bc.context(outer, bc.call(b.name('SelectScriptedTarget')),
                                         b.ref('GearGame.GearAI.SelectScriptedTarget.ReturnValue'), call_only=True)),
        bc.context(driver, bc.call(b.name('SetScriptedFireTarget'), fire_target, bc.false(), bc.float_const(3.0))),
        bc.context(driver, bc.call(b.name('FireRockets'), outer)),
        bc.label('pop'),
        bc.context(outer, bc.call(b.name('PopCommand'), bc.self_()), call_only=True),
        bc.stop())


# Not Vehicle_Reaver_Base's GetHitAnimName_Damage, PlayHitReactionAnimation and
# ShouldTurnCursorRedFor, which the DLC's Raam Reaver overrides: the disc build's loader breaks on a
# class that receives a new property (TaccomMesh) and new functions. The DLC's overrides keep no
# parent function, and the runtime takes their Super calls (runtime/src/dlc/missing_super.h).

# Bytecode tokens.
EX_LOCAL_VARIABLE = 0x00
EX_RETURN = 0x04
EX_NOTHING = 0x0B
EX_END_FUNCTION_PARMS = 0x16
EX_FINAL_FUNCTION = 0x1C
EX_END_OF_SCRIPT = 0x53
CPF_RETURN_PARM = 0x400
CPF_PARM = 0x80
CPF_NEED_CTOR_LINK = 0x400000
FUNC_FINAL = 0x1
FUNC_NATIVE = 0x400

# --- Building ------------------------------------------------------------------------------------


class Builder:
    def __init__(self, pkg, engine=None):
        self.pkg = pkg
        self.engine = engine  # the Engine package, for the default objects of Engine parents
        self.patch = Patch('GearGame')
        self.blobs = []
        self.next_net_index = pkg.net_object_count
        self.none = pkg.name_index['None']

    # Names: the package's, then the patch's own.
    def name(self, text):
        if text in self.pkg.name_index:
            return self.pkg.name_index[text]
        if text not in self.patch.names:
            self.patch.names.append(text)
        return len(self.pkg.names) + self.patch.names.index(text)

    def ref(self, path):
        """Reference to an object of the package (or one of the patch's own, by path)."""
        if path.startswith('GearGame.'):
            local = path[len('GearGame.'):]
            for k, e in enumerate(self.patch.exports):
                if self.export_path(k) == local:
                    return len(self.pkg.exports) + 1 + k
            index = self.pkg.find(local)
        else:
            index = self.pkg.find(path)
        if not index:
            raise KeyError(f'{path} is not in GearGame')
        return index

    def export_path(self, k):
        e = self.patch.exports[k]
        parts = [e['name']]
        outer = e['outer']
        while outer:
            if outer > len(self.pkg.exports):
                o = self.patch.exports[outer - len(self.pkg.exports) - 1]
                parts.append(o['name']); outer = o['outer']
            else:
                parts.append(self.pkg.path(outer)); break
        return '.'.join(reversed(parts))

    def add(self, name, cls, outer, flags, super_=0, archetype=0):
        self.patch.exports.append(dict(cls=cls, super=super_, outer=outer, name=name, archetype=archetype, flags=flags))
        self.blobs.append(None)
        self.name(name)
        return len(self.pkg.exports) + len(self.patch.exports)

    def net_index(self):
        n = self.next_net_index
        self.next_net_index += 1
        return n

    def set_blob(self, index, data):
        self.blobs[index - len(self.pkg.exports) - 1] = data

    def blob(self, index):
        return self.blobs[index - len(self.pkg.exports) - 1]

    # Properties: NetIndex, no tagged properties (None), Next, ArrayDim, PropertyFlags, then the
    # type's references.
    def property(self, outer, name, kind, flags, extra):
        if kind == 'ArrayProperty':
            # The inner property comes first, with the array (the next export) as its outer, as the
            # script compiler exports them; arrays need constructor linking (CPF_NeedCtorLink).
            inner_kind, inner_flags, *inner_extra = extra
            inner_extra = inner_extra[0] if len(inner_extra) == 1 else tuple(inner_extra)
            array = len(self.pkg.exports) + len(self.patch.exports) + 2
            inner = self.property(array, name, inner_kind, inner_flags, inner_extra)
            index = self.add(name, self.ref('Core.' + kind), outer, ue3.RF_PUBLIC_LOADED)
            assert index == array
            w = Writer()
            w.i32(self.net_index()); w.name(self.none); w.i32(0); w.i32(1); w.u64(flags | CPF_NEED_CTOR_LINK)
            w.i32(inner)
            self.set_blob(index, bytes(w.b))
            return index
        index = self.add(name, self.ref('Core.' + kind), outer, ue3.RF_PUBLIC_LOADED)
        w = Writer()
        w.i32(self.net_index()); w.name(self.none); w.i32(0); w.i32(1); w.u64(flags)
        if kind in ('ObjectProperty', 'ComponentProperty'):
            w.i32(self.ref(extra))
        elif kind == 'ClassProperty':
            w.i32(self.ref(extra[0])); w.i32(self.ref(extra[1]))
        elif kind not in ('BoolProperty', 'IntProperty', 'FloatProperty'):
            raise ValueError(kind)
        self.set_blob(index, bytes(w.b))
        return index

    def link(self, fields):
        """Chains fields through UField::Next (at +12 of their data)."""
        for a, b in zip(fields, fields[1:]):
            data = bytearray(self.blob(a)); struct.pack_into('>i', data, 12, b); self.set_blob(a, bytes(data))

    def parent_info(self, parent):
        """Class data to inherit from the parent: a GearGame class, or a GearGame class that extends
        the same Engine class (for flags, probe mask and state flags)."""
        if parent.startswith('Engine.'):
            super_ = self.ref(parent)
            sibling = next(i for i, e in enumerate(self.pkg.exports, 1) if e['cls'] == 0 and e['super'] == super_)
            info = parse_class(self.pkg.data(sibling))
            info['components'] = self.import_components(parent)
            return super_, info
        super_ = self.pkg.find_class(parent)
        info = parse_class(self.pkg.data(super_))
        info['components'] = [(self.pkg.text(n), o) for n, o in info['components']]
        return super_, info

    def subobject_exports(self, default):
        """(name, export index) of the patch's subobjects of `default`."""
        return [(e['name'], len(self.pkg.exports) + 1 + k) for k, e in enumerate(self.patch.exports)
                if e['outer'] == default]

    def default_tags(self, w, parent, ours):
        """The parent default object's properties that point at its subobjects, pointing at ours.

        A default object's component properties (Mesh, CylinderComponent, Components...) name the
        subobjects it owns; the new class's default object must name its own copies, or every object
        of the class would share its parent's templates."""
        if parent.startswith('Engine.'):
            pkg, default = self.engine, self.engine.find('Default__' + parent.split('.')[1])
        else:
            pkg, default = self.pkg, self.pkg.find('Default__' + parent)
        if not pkg or not default:
            raise KeyError(f'no default object for {parent}')
        owned = {i: pkg.exports[i - 1]['name'] for i in pkg.children(default)}
        tags, _ = ue3.parse_tags(pkg, pkg.data(default), 4)
        for header, name, kind, value in tags:
            if kind in ('ObjectProperty', 'ComponentProperty') and len(value) == 4:
                target = struct.unpack('>i', value)[0]
                if target in owned and owned[target] in ours:
                    self.tag(w, header, pkg, name, kind, struct.pack('>i', ours[owned[target]]))
            elif kind == 'ArrayProperty' and len(value) >= 4:
                count = struct.unpack_from('>i', value, 0)[0]
                if len(value) != 4 + 4 * count:
                    continue
                items = list(struct.unpack_from(f'>{count}i', value, 4))
                if not any(i in owned and owned[i] in ours for i in items):
                    continue
                if pkg is not self.pkg and not all(i in owned for i in items):
                    continue  # other Engine objects: not referable from GearGame
                # The parent's own subobjects become ours; anything else stays as it is.
                remapped = [ours[owned[i]] if i in owned and owned[i] in ours else i for i in items]
                self.tag(w, header, pkg, name, kind, struct.pack(f'>i{count}i', count, *remapped))

    def tagged(self, tag):
        """A tagged property of ours (see CLASSES): name, type, size, array index 0, the type's extra
        and the value."""
        kind, name = tag[0], tag[1]
        w, value = Writer(), Writer()
        if kind == 'float':
            type_name = 'FloatProperty'; value.raw(struct.pack('>f', tag[2]))
        elif kind == 'bool':
            type_name = 'BoolProperty'
        elif kind == 'name':
            type_name = 'NameProperty'; value.name(self.name(tag[2]))
        elif kind == 'enum':
            type_name = 'ByteProperty'; value.name(self.name(tag[3]))
        elif kind == 'struct':
            type_name = 'StructProperty'
            for inner in tag[3]:
                value.raw(self.tagged(inner))
            value.name(self.none)
        elif kind == 'structs':
            type_name = 'ArrayProperty'; value.i32(len(tag[2]))
            for element in tag[2]:
                for inner in element:
                    value.raw(self.tagged(inner))
                value.name(self.none)
        else:
            raise ValueError(kind)
        w.name(self.name(name)); w.name(self.name(type_name)); w.i32(len(value.b)); w.i32(0)
        if kind == 'bool':
            w.u8(1 if tag[2] else 0)
        elif kind == 'struct':
            w.name(self.name(tag[2]))
        elif kind == 'enum':
            w.name(self.name(tag[2]))
        w.raw(bytes(value.b))
        return bytes(w.b)

    def tag(self, w, header, pkg, name, kind, value):
        """Writes a tagged property: name, type, size, array index, value (names in this patch)."""
        array_index = struct.unpack_from('>i', header, 20)[0]
        w.name(self.name(name)); w.name(self.name(kind)); w.i32(len(value)); w.i32(array_index); w.raw(value)

    def subobjects(self, parent):
        """(name, object) of every object the parent class's default object owns."""
        if parent.startswith('Engine.'):
            return self.import_components(parent)
        default = self.pkg.find('Default__' + parent)
        return [(self.pkg.exports[i - 1]['name'], i) for i in self.pkg.children(default)]

    def import_components(self, parent):
        """Component templates of an Engine class's default object, as GearGame imports them."""
        cls = parent.split('.')[1]
        default = self.pkg.find(f'Engine.Default__{cls}')
        return [(imp['name'], -k - 1) for k, imp in enumerate(self.pkg.imports) if default and imp['outer'] == default]

    def is_a(self, template, class_name):
        """Whether the component `template` is a `class_name` or a subclass of it."""
        o = self.pkg.obj(template)
        if template < 0:
            return o['class_name'] == class_name or o['class_name'].endswith(class_name)
        return self.pkg.obj_name(o['cls']) == class_name

    def component_class(self, template):
        o = self.pkg.obj(template)
        if template < 0:
            return self.ref(f"{o['class_package']}.{o['class_name']}")
        return o['cls']

    def new_class(self, name, parent, properties, extra=None):
        extra = extra or {}
        super_, info = self.parent_info(parent)
        cls = self.add(name, 0, 0, ue3.RF_PUBLIC_LOADED | ue3.RF_STANDALONE, super_)
        default = self.add('Default__' + name, cls, 0, ue3.RF_PUBLIC_LOADED | ue3.RF_CLASS_DEFAULT_OBJECT,
                           archetype=self.ref(f"{'Engine' if parent.startswith('Engine.') else 'GearGame'}.Default__{parent.split('.')[-1]}"))
        # Subobjects: a copy of each of the parent default object's (components, and other objects it
        # owns such as camera shakes), which they take as archetype. The DLC's packages import them
        # by path.
        component_names = {n for n, _ in info['components']}
        components = []
        for sub_name, template in self.subobjects(parent):
            c = self.add(sub_name, self.component_class(template), default, ue3.RF_PUBLIC_LOADED, archetype=template)
            w = Writer()
            if sub_name in component_names:
                # A component: its template's owner class (none) and name, then its properties.
                w.i32(0); w.name(self.name(sub_name))
            w.i32(self.net_index()); w.name(self.none)
            if self.is_a(template, 'StaticMeshComponent') or self.is_a(template, 'BrushComponent'):
                w.i32(0)  # the component's own data after its properties: an empty array
            self.set_blob(c, bytes(w.b))
            if sub_name in component_names:
                components.append((sub_name, c))
        w = Writer()
        w.i32(self.net_index())
        self.default_tags(w, parent, {name: c for name, c in self.subobject_exports(default)})
        for tag in extra.get('defaults', []):
            w.raw(self.tagged(tag))
        w.name(self.none)
        self.set_blob(default, bytes(w.b))
        fields = [self.property(cls, *p) for p in properties]
        # Functions and states, chained as the Title Update's classes have them: the states, then the
        # functions from the last declared to the first, then the properties.
        functions = [self.function(cls, *f) for f in extra.get('functions', [])]
        states = [self.state(cls, state_name, script) for state_name, script in extra.get('states', [])]
        fields = states + list(reversed(functions)) + fields
        self.link(fields)
        # Class data: no script; flags of a script class.
        flags = info['class_flags'] & ~(ue3.CLASS_NATIVE | ue3.CLASS_NATIVE_REPLICATION | ue3.CLASS_EXPORTED |
                                        ue3.CLASS_ABSTRACT | ue3.CLASS_NOEXPORT | ue3.CLASS_INTRINSIC)
        flags |= ue3.CLASS_COMPILED | ue3.CLASS_PARSED
        w = Writer()
        w.i32(self.net_index()); w.i32(0); w.i32(super_); w.i32(fields[0] if fields else 0)
        w.i32(0); w.i32(0)
        w.u32(info['probe_mask']); w.u16(0xFFFF); w.u32(info['state_flags'])
        # Function map: each function by name, as the engine finds them.
        w.u32(len(functions))
        for f in reversed(functions):
            w.name(self.name(self.patch.exports[f - len(self.pkg.exports) - 1]['name'])); w.i32(f)
        within = self.pkg.find_class(extra['within']) if 'within' in extra else info['within']
        w.u32(flags); w.i32(within); w.name(*info['config_name'])
        w.u32(len(components))
        for component_name, c in components:
            w.name(self.name(component_name)); w.i32(c)
        w.u32(0)  # interfaces
        w.name(self.none)
        w.i32(default)
        self.set_blob(cls, bytes(w.b))

    def member(self, owner, name, kind, flags, extra):
        cls = self.pkg.find_class(owner)
        index = self.property(cls, name, kind, flags, extra)
        # The disc build's loader links a patch property into its existing class (as its first
        # field, before the one its Next names) and lays the class out again. So that no field the
        # executable knows moves, the property has no elements (ArrayDim 0): it takes no room, and
        # the engine's copying and serialization, which go through ArrayDim elements, leave it
        # alone. An array also leaves the constructor link (no CPF_NeedCtorLink), which the
        # engine's constructors and destructors follow whatever the ArrayDim. The runtime gives the
        # property a place afterwards (runtime/src/dlc/class_fixups.h); script reaches it by its
        # offset only. Next names the class's first field, so the class keeps all of them.
        data = bytearray(self.blob(index))
        struct.pack_into('>i', data, 12, parse_class(self.pkg.data(cls))['children'])
        struct.pack_into('>i', data, 16, 0)
        flags = struct.unpack_from('>Q', data, 20)[0]
        struct.pack_into('>Q', data, 20, flags & ~CPF_NEED_CTOR_LINK)
        self.set_blob(index, bytes(data))

    def chain(self, struct_index):
        """Fields of a disc function, in order (Children, then each field's Next)."""
        fields = []
        field = struct.unpack_from('>i', self.pkg.data(struct_index), 20)[0]
        while field:
            fields.append(field)
            field = struct.unpack_from('>i', self.pkg.data(field), 12)[0]
        return fields

    def param(self, function, source):
        if isinstance(source, tuple):
            name, kind, *extra = source
            flags = extra[1] if len(extra) > 1 else CPF_PARM
            p = self.add(name, self.ref('Core.' + kind), function, ue3.RF_PUBLIC_LOADED)
            w = Writer()
            w.i32(self.net_index()); w.name(self.none); w.i32(0); w.i32(1); w.u64(flags)
            if kind == 'ObjectProperty':
                w.i32(self.ref(extra[0]))
            self.set_blob(p, bytes(w.b))
            return p
        model = self.pkg.find(source)
        if not model:
            raise KeyError(source)
        e = self.pkg.exports[model - 1]
        p = self.add(e['name'], e['cls'], function, ue3.RF_PUBLIC_LOADED)
        copy = bytearray(self.pkg.data(model))
        struct.pack_into('>i', copy, 0, self.net_index())
        struct.pack_into('>i', copy, 12, 0)
        self.set_blob(p, bytes(copy))
        return p

    def state(self, owner, name, script):
        """A state of the new class `owner`: its code (from its 'Begin' label), label table, no
        functions of its own."""
        state = self.add(name, self.ref('Core.State'), owner, ue3.RF_PUBLIC_LOADED)
        code, memory_size, label_table = bc.assemble_state(script(self), {'Begin': self.name('Begin')}, self.none)
        w = Writer()
        w.i32(self.net_index()); w.name(self.none); w.i32(0); w.i32(0); w.i32(0)
        w.i32(memory_size); w.i32(len(code)); w.raw(code)
        w.u32(0); w.u16(label_table); w.u32(0)  # probe mask, label table offset, state flags
        w.u32(0)  # function map
        self.set_blob(state, bytes(w.b))
        return state

    def function(self, owner, name, like, params, body):
        """A function of `owner`: a disc class's name, or the export index of a new class (then its
        Next is set when the class's fields are chained)."""
        model = self.pkg.find(like)
        if not model:
            raise KeyError(like)
        data = self.pkg.data(model)
        r = ue3.Reader(data)
        r.i32(); r.name(); r.i32(); r.i32(); r.i32()  # NetIndex, None, Next, Super, Children
        model_memory_size = r.i32()
        model_code = r.take(r.i32())
        tail = bytearray(data[r.p:])
        # Native index (16 bits), operator precedence (8), function flags (32) and, for replicated
        # functions, the replication offset. Not native and not final: the DLC overrides them.
        struct.pack_into('>H', tail, 0, 0)
        struct.pack_into('>I', tail, 3, struct.unpack_from('>I', tail, 3)[0] & ~(FUNC_FINAL | FUNC_NATIVE))
        new_owner = isinstance(owner, int)
        owner_index = owner if new_owner else self.pkg.find_class(owner)
        function = self.add(name, self.ref('Core.Function'), owner_index, ue3.RF_PUBLIC_LOADED)
        if isinstance(params, str):  # the parameters of a disc function, all of them
            sources = [self.pkg.path(f) for f in self.chain(self.pkg.find(params))
                       if struct.unpack_from('>Q', self.pkg.data(f), 20)[0] & CPF_PARM]
        else:
            sources = params
        fields = [self.param(function, source) for source in sources]
        self.link(fields)
        references = 0
        memory_size = None
        if body == 'empty':
            code = bytes([EX_RETURN, EX_NOTHING, EX_END_OF_SCRIPT])
        elif body[0] == 'copy':
            # The model's bytecode, references replaced: the same layout, so the same size in memory.
            code = bytearray(model_code)
            params = {self.export_path(f - len(self.pkg.exports) - 1).split('.')[-1]: f for f in fields}
            for path, replacement in body[1].items():
                old = struct.pack('>i', self.pkg.find(path))
                new = struct.pack('>i', params[replacement] if replacement in params else self.ref(replacement))
                if code.count(old) == 0:
                    raise ValueError(f'{like} does not name {path}')
                code = code.replace(old, new)
            code = bytes(code)
            memory_size = model_memory_size
        elif body[0] == 'script':
            names = {self.export_path(f - len(self.pkg.exports) - 1).split('.')[-1]: f for f in fields}
            code, memory_size = bc.assemble(body[1](self, names))
        else:
            # return Super.<Function>(<parameters>);
            w = Writer()
            w.u8(EX_RETURN); w.u8(EX_FINAL_FUNCTION); w.i32(self.ref(body[1]))
            references += 1
            for f in fields:
                if not struct.unpack_from('>Q', self.blob(f), 20)[0] & CPF_RETURN_PARM:
                    w.u8(EX_LOCAL_VARIABLE); w.i32(f)
                    references += 1
            w.u8(EX_END_FUNCTION_PARMS); w.u8(EX_END_OF_SCRIPT)
            code = bytes(w.b)
        w = Writer()
        # Next names the class's first field: the disc build's loader may link the function into the
        # class along with the class's new properties (see member()), and the class keeps its fields.
        first = 0 if new_owner else parse_class(self.pkg.data(owner_index))['children']
        w.i32(self.net_index()); w.name(self.none); w.i32(first); w.i32(0); w.i32(fields[0] if fields else 0)
        # Bytecode size in memory, where each object reference takes 8 bytes (4 in the file), then
        # in the file.
        w.i32(memory_size if memory_size is not None else len(code) + 4 * references)
        w.i32(len(code)); w.raw(code); w.raw(bytes(tail))
        self.set_blob(function, bytes(w.b))
        return function

    def build(self, only=None):
        # `only` (names of classes, members or functions) limits the patch, to find problems.
        keep = (lambda name: only is None or name in only)
        for name, parent, properties, *extra in CLASSES:
            if keep(name):
                self.new_class(name, parent, properties, *extra)
        for member in MEMBERS:
            if keep(member[1]):
                self.member(*member)
        for function in FUNCTIONS:
            if keep(function[1]):
                self.function(*function)
        self.patch.new_objects = [(e['name'], b) for e, b in zip(self.patch.exports, self.blobs)]
        return self.patch.write()


def main():
    game, out = sys.argv[1], sys.argv[2]
    xxx = os.path.join(game, 'GearGame', 'CookedXbox360', 'GearGame.xxx')
    raw = open(xxx, 'rb').read()
    # Decompressing GearGame.xxx takes a few minutes; --uncompressed reuses an earlier result.
    cache = sys.argv[sys.argv.index('--uncompressed') + 1] if '--uncompressed' in sys.argv else None
    if cache and os.path.exists(cache):
        data = open(cache, 'rb').read()
    else:
        data = ue3.read_game_package(xxx)
        if cache:
            open(cache, 'wb').write(data)
    pkg = ue3.Package(data)
    engine = ue3.Package(ue3.read_game_package(os.path.join(game, 'GearGame', 'CookedXbox360', 'Engine.xxx')))
    only = set(sys.argv[sys.argv.index('--only') + 1].split(',')) if '--only' in sys.argv else None
    data = Builder(pkg, engine).build(only)
    folder = os.path.join(out, 'GearGame', 'Patches')
    os.makedirs(folder, exist_ok=True)
    open(os.path.join(folder, 'ScriptPatch_GearGame.bin'), 'wb').write(ue3.pack(data))
    open(os.path.join(folder, 'ScriptPatch_GearGame.bin_us'), 'wb').write(b'%d\r\n' % len(data))
    json.dump({'GearGame.xxx': hashlib.sha256(raw).hexdigest(), 'patch_size': len(data)},
              open(os.path.join(out, 'dlc_patch.json'), 'w'), indent=2)
    print(f'{len(data)} bytes')


if __name__ == '__main__':
    main()
