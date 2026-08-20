from __future__ import annotations

import re
import tomllib
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HOOK_CONFIG = ROOT / "rerevved_hooks.toml"
HOOK_SOURCE = ROOT / "src" / "compat_hooks.cpp"
GENERATED = ROOT / "generated" / "default"

EXPECTED_HOOKS = [
    {
        "address": 0x82E60F70,
        "name": "ReRevvedProbeGameStart",
    },
    {
        "address": 0x82C7DF58,
        "name": "ReRevvedProbeGameplayFrame",
    },
    {
        "address": 0x82CE1950,
        "name": "ReRevvedProbeUnitMoveSubmit",
        "registers": ["r3", "r4", "r5", "r6", "r7", "r8"],
    },
    {
        "address": 0x82CDEDFC,
        "name": "ReRevvedProbeUnitMoveApplyBegin",
        "registers": ["r31", "r15", "r29"],
    },
    {
        "address": 0x82CDEE84,
        "name": "ReRevvedProbeUnitMoveMapUpdateBegin",
    },
    {
        "address": 0x82CDEE88,
        "name": "ReRevvedProbeUnitMoveMapUpdateEnd",
    },
    {
        "address": 0x82CDF134,
        "name": "ReRevvedProbeUnitMovePresentationBegin",
        "registers": ["ctr"],
    },
    {
        "address": 0x82CDF138,
        "name": "ReRevvedProbeUnitMovePresentationReturned",
        "registers": ["r3"],
    },
    {
        "address": 0x82CDF164,
        "name": "ReRevvedProbeUnitMovePresentationPoll",
    },
    {
        "address": 0x82CDF184,
        "name": "ReRevvedProbeUnitMovePresentationEnd",
    },
    {
        "address": 0x82CDF324,
        "name": "ReRevvedProbeUnitMovePresentationBegin",
        "registers": ["ctr"],
    },
    {
        "address": 0x82CDF328,
        "name": "ReRevvedProbeUnitMovePresentationReturned",
        "registers": ["r3"],
    },
    {
        "address": 0x82CDF340,
        "name": "ReRevvedProbeUnitMovePresentationPoll",
    },
    {
        "address": 0x82CDF36C,
        "name": "ReRevvedProbeUnitMovePresentationEnd",
    },
    {
        "address": 0x82CD7580,
        "name": "ReRevvedProbeUnitMoveDurationSet",
        "registers": ["r11"],
    },
    {
        "address": 0x82CD758C,
        "name": "ReRevvedProbeUnitMoveAnimationBegin",
    },
    {
        "address": 0x82CD7590,
        "name": "ReRevvedProbeUnitMovePresentationPoll",
    },
    {
        "address": 0x82CD75C4,
        "name": "ReRevvedProbeUnitMovePresentationEnd",
    },
    {
        "address": 0x82CE16AC,
        "name": "ReRevvedProbeUnitMoveApplyEnd",
    },
    {
        "address": 0x82CDF74C,
        "name": "ReRevvedProbeCombatApplyBegin",
        "registers": ["r31", "r15", "r29"],
    },
    {
        "address": 0x82CD9970,
        "name": "ReRevvedProbeCombatResolveBegin",
        "registers": ["r3", "r4", "r5", "r6", "r7"],
    },
    {
        "address": 0x82CD9C08,
        "name": "ReRevvedProbeCombatParticipants",
        "registers": ["r1"],
    },
    {
        "address": 0x82CDCB34,
        "name": "ReRevvedProbeCombatAuxPresentation",
        "registers": ["r3", "r4", "r1"],
    },
    {
        "address": 0x82CDCDB8,
        "name": "ReRevvedProbeCombatAuxPresentation",
        "registers": ["r3", "r4", "r1"],
    },
    {
        "address": 0x82CDCEB4,
        "name": "ReRevvedProbeCombatAuxPresentation",
        "registers": ["r3", "r4", "r1"],
    },
    {
        "address": 0x82CDD1A4,
        "name": "ReRevvedProbeCombatAuxPresentation",
        "registers": ["r3", "r4", "r1"],
    },
    {
        "address": 0x82CDD930,
        "name": "ReRevvedProbeCombatPresentationBegin",
        "registers": ["ctr", "r1"],
    },
    {
        "address": 0x82CDD958,
        "name": "ReRevvedProbeCombatPresentationPoll",
    },
    {
        "address": 0x82CDD978,
        "name": "ReRevvedProbeCombatPresentationEnd",
        "registers": ["r1"],
    },
    {
        "address": 0x82CDDEF8,
        "name": "ReRevvedProbeCombatResolveEnd",
        "registers": ["r1"],
    },
    {
        "address": 0x8269CAE0,
        "name": "ReRevvedCompatRingInitializeBegin",
        "registers": ["r3", "r4"],
    },
    {
        "address": 0x8269CAE4,
        "name": "ReRevvedCompatRingInitializeEnd",
    },
    {
        "address": 0x82245050,
        "name": "ReRevvedRememberGfxRenderConfig",
        "registers": ["r3", "r4"],
    },
    {
        "address": 0x82302E90,
        "name": "ReRevvedHandleGfxRenderCapsBegin",
        "registers": ["r3", "r4", "lr"],
    },
    {
        "address": 0x82302F0C,
        "name": "ReRevvedHandleGfxRenderCapsEnd",
        "registers": ["r3", "r31"],
    },
    {
        "address": 0x82253D2C,
        "name": "ReRevvedCompatExpandGfxVectorGlyphCache",
        "registers": ["r31"],
    },
]


class HookContractTests(unittest.TestCase):
    def test_only_verified_hooks_are_configured(self) -> None:
        with HOOK_CONFIG.open("rb") as stream:
            config = tomllib.load(stream)

        self.assertEqual(config["midasm_hook"], EXPECTED_HOOKS)

    def test_configured_names_match_compat_hook_functions(self) -> None:
        with HOOK_CONFIG.open("rb") as stream:
            config = tomllib.load(stream)
        source = HOOK_SOURCE.read_text(encoding="utf-8")
        source_names = set(
            re.findall(r"^void (ReRevved\w+)\s*\(", source, re.MULTILINE)
        )
        hook_names = {hook["name"] for hook in config["midasm_hook"]}

        self.assertEqual(
            source_names - {"ReRevvedCompatNullOptionalDispatch"},
            hook_names,
        )

    def test_generated_ring_hook_placement_when_available(self) -> None:
        paths = sorted(GENERATED.glob("rerevved_recomp.*.cpp"))
        if not paths:
            self.skipTest("generated sources are not available")

        generated = "".join(path.read_text(encoding="utf-8") for path in paths)
        expected = (
            "// bl 0x82e923e4\n"
            "\tReRevvedCompatRingInitializeBegin(ctx.r3, ctx.r4);\n"
            "\tctx.lr = 0x8269CAE4;\n"
            "\t__imp__VdInitializeRingBuffer(ctx, base);\n"
            "\t// rlwinm r11,r25,23,9,31\n"
            "\tReRevvedCompatRingInitializeEnd();"
        )
        self.assertEqual(generated.count(expected), 1)

    def test_generated_gameplay_probe_placement_when_available(self) -> None:
        paths = sorted(GENERATED.glob("rerevved_recomp.*.cpp"))
        if not paths:
            self.skipTest("generated sources are not available")

        generated = "".join(path.read_text(encoding="utf-8") for path in paths)
        expected = {
            "sub_82E60F70": "ReRevvedProbeGameStart();",
            "sub_82C7DF58": "ReRevvedProbeGameplayFrame();",
        }
        for function, hook in expected.items():
            placement = (
                f"DEFINE_REX_FUNC({function}) {{\n"
                "\tREX_FUNC_PROLOGUE();\n"
                "\tuint32_t ea{};\n"
                "\t// mflr r12\n"
                f"\t{hook}"
            )
            self.assertEqual(generated.count(placement), 1)

    def test_generated_movement_probe_placement_when_available(self) -> None:
        paths = sorted(GENERATED.glob("rerevved_recomp.*.cpp"))
        if not paths:
            self.skipTest("generated sources are not available")

        generated = "".join(path.read_text(encoding="utf-8") for path in paths)
        expected = {
            "DEFINE_REX_FUNC(sub_82CE1950) {\n": (
                "\tREX_FUNC_PROLOGUE();\n"
                "\tuint32_t ea{};\n"
                "\t// mflr r12\n"
                "\tReRevvedProbeUnitMoveSubmit(ctx.r3, ctx.r4, ctx.r5, ctx.r6, ctx.r7, ctx.r8);"
            ),
            "loc_82CDEDFC:\n": (
                "\t// lis r11,-31985\n"
                "\tReRevvedProbeUnitMoveApplyBegin(ctx.r31, ctx.r15, ctx.r29);"
            ),
            "\t// bl 0x82d076d8\n": (
                "\tReRevvedProbeUnitMoveMapUpdateBegin();\n"
                "\tctx.lr = 0x82CDEE88;"
            ),
            "\t// lis r10,-31979\n": (
                "\tReRevvedProbeUnitMoveMapUpdateEnd();\n"
                "\tctx.r10.s64 = -2095775744;"
            ),
            "\t// bctrl \n": (
                "\tReRevvedProbeUnitMovePresentationBegin(ctx.ctr);\n"
                "\tctx.lr = 0x82CDF138;"
            ),
            "\t// mr r24,r3\n": (
                "\tReRevvedProbeUnitMovePresentationReturned(ctx.r3);"
            ),
            "loc_82CDF164:\n": (
                "\t// li r5,1\n"
                "\tReRevvedProbeUnitMovePresentationPoll();"
            ),
            "loc_82CDF184:\n": (
                "\t// rlwinm r11,r11,0,24,22\n"
                "\tReRevvedProbeUnitMovePresentationEnd();"
            ),
            "loc_82CE16AC:\n": (
                "\t// lis r10,-31979\n"
                "\tReRevvedProbeUnitMoveApplyEnd();"
            ),
        }
        for anchor, placement in expected.items():
            self.assertEqual(generated.count(anchor + placement), 1)

        alternate = [
            (
                "\t// bctrl \n"
                "\tReRevvedProbeUnitMovePresentationBegin(ctx.ctr);\n"
                "\tctx.lr = 0x82CDF328;"
            ),
            (
                "\t// lwz r11,-6204(r31)\n"
                "\tReRevvedProbeUnitMovePresentationReturned(ctx.r3);"
            ),
            (
                "loc_82CDF340:\n"
                "\t// li r5,1\n"
                "\tReRevvedProbeUnitMovePresentationPoll();"
            ),
            (
                "loc_82CDF36C:\n"
                "\t// lwz r11,0(r30)\n"
                "\tReRevvedProbeUnitMovePresentationEnd();"
            ),
        ]
        for placement in alternate:
            self.assertEqual(generated.count(placement), 1)

        ordinary = [
            (
                "\t// stw r11,17736(r10)\n"
                "\tReRevvedProbeUnitMoveDurationSet(ctx.r11);"
            ),
            (
                "\t// bl 0x82d11ad8\n"
                "\tReRevvedProbeUnitMoveAnimationBegin();\n"
                "\tctx.lr = 0x82CD7590;"
            ),
            (
                "loc_82CD7590:\n"
                "\t// lis r11,-32000\n"
                "\tReRevvedProbeUnitMovePresentationPoll();"
            ),
            (
                "loc_82CD75C4:\n"
                "\t// addi r11,r22,34\n"
                "\tReRevvedProbeUnitMovePresentationEnd();"
            ),
        ]
        for placement in ordinary:
            self.assertEqual(generated.count(placement), 1)

    def test_generated_combat_probe_placement_when_available(self) -> None:
        paths = sorted(GENERATED.glob("rerevved_recomp.*.cpp"))
        if not paths:
            self.skipTest("generated sources are not available")

        generated = "".join(path.read_text(encoding="utf-8") for path in paths)
        placements = [
            (
                "loc_82CDF74C:\n"
                "\t// lis r11,-31985\n"
                "\tReRevvedProbeCombatApplyBegin(ctx.r31, ctx.r15, ctx.r29);"
            ),
            (
                "DEFINE_REX_FUNC(sub_82CD9970) {\n"
                "\tREX_FUNC_PROLOGUE();\n"
                "\tPPCRegister temp{};\n"
                "\tuint32_t ea{};\n"
                "\t// mflr r12\n"
                "\tReRevvedProbeCombatResolveBegin(ctx.r3, ctx.r4, ctx.r5, ctx.r6, ctx.r7);"
            ),
            (
                "loc_82CD9C08:\n"
                "\t// cmpwi cr6,r26,-1\n"
                "\tReRevvedProbeCombatParticipants(ctx.r1);"
            ),
            (
                "\t// bl 0x82d11ad8\n"
                "\tReRevvedProbeCombatAuxPresentation(ctx.r3, ctx.r4, ctx.r1);\n"
                "\tctx.lr = 0x82CDCB38;"
            ),
            (
                "\t// bl 0x82d11ad8\n"
                "\tReRevvedProbeCombatAuxPresentation(ctx.r3, ctx.r4, ctx.r1);\n"
                "\tctx.lr = 0x82CDCDBC;"
            ),
            (
                "\t// bl 0x82d11ad8\n"
                "\tReRevvedProbeCombatAuxPresentation(ctx.r3, ctx.r4, ctx.r1);\n"
                "\tctx.lr = 0x82CDCEB8;"
            ),
            (
                "\t// bl 0x82d11ad8\n"
                "\tReRevvedProbeCombatAuxPresentation(ctx.r3, ctx.r4, ctx.r1);\n"
                "\tctx.lr = 0x82CDD1A8;"
            ),
            (
                "\t// bctrl \n"
                "\tReRevvedProbeCombatPresentationBegin(ctx.ctr, ctx.r1);\n"
                "\tctx.lr = 0x82CDD934;"
            ),
            (
                "loc_82CDD958:\n"
                "\t// li r5,1\n"
                "\tReRevvedProbeCombatPresentationPoll();"
            ),
            (
                "loc_82CDD978:\n"
                "\t// lwzx r10,r16,r30\n"
                "\tReRevvedProbeCombatPresentationEnd(ctx.r1);"
            ),
            (
                "loc_82CDDEF8:\n"
                "\t// li r3,0\n"
                "\tReRevvedProbeCombatResolveEnd(ctx.r1);"
            ),
        ]
        for placement in placements:
            self.assertEqual(generated.count(placement), 1)

    def test_generated_vector_glyph_cache_hook_when_available(self) -> None:
        paths = sorted(GENERATED.glob("rerevved_recomp.*.cpp"))
        if not paths:
            self.skipTest("generated sources are not available")

        generated = "".join(path.read_text(encoding="utf-8") for path in paths)
        expected = (
            "\tctx.lr = 0x82253D2C;\n"
            "\tsub_82253DC8(ctx, base);\n"
            "\t// mr r3,r31\n"
            "\tReRevvedCompatExpandGfxVectorGlyphCache(ctx.r31);"
        )
        self.assertEqual(generated.count(expected), 1)


if __name__ == "__main__":
    unittest.main()
