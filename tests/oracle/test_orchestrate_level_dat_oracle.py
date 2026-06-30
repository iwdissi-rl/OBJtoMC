#!/usr/bin/env python3
"""orchestrate_level_dat_oracle — Python oracle for proposal §3.13(c).

Drives the C++ side (test_orchestrate_level_dat_writer via PATH_TO_BINARY)
to emit a level.dat with deterministic LastPlayed=0, then reads it back
via twoolie/NBT==1.5.1 and asserts every field per the proposal §3.5
schema plus the Session 26 §3.5 implementation-time-verification
additions documented in research/scratch/orchestration/level_dat_diff.md.

Exits 0 on PASS, 1 on FAIL with the first failing assertion's message.
"""

import os
import subprocess
import sys
import tempfile

import nbt.nbt as nbt


def fail(msg):
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def expect(actual, expected, label):
    if actual != expected:
        fail(f"{label}: expected {expected!r}, got {actual!r}")


def expect_in(needle, haystack, label):
    if needle not in haystack:
        fail(f"{label}: {needle!r} not found in {label}")


def main():
    binary = os.environ.get("PATH_TO_BINARY")
    if not binary:
        fail("PATH_TO_BINARY env var not set")

    with tempfile.TemporaryDirectory() as td:
        out_path = os.path.join(td, "level.dat")
        result = subprocess.run([binary, out_path], capture_output=True, text=True)
        if result.returncode != 0:
            fail(f"writer exited {result.returncode}; stderr={result.stderr}")

        # twoolie/NBT auto-detects gzip from the magic bytes; passing a file
        # via filename triggers the gzip path.
        nbt_tree = nbt.NBTFile(filename=out_path)
        data = nbt_tree["Data"]

    # ===== §3.5 minimum-viable schema fields =====
    expect(data["DataVersion"].value, 4325, "DataVersion")
    expect(data["version"].value, 19133, "version")

    v = data["Version"]
    expect(v["Id"].value, 4325, "Version.Id")
    expect(v["Name"].value, "1.21.5", "Version.Name")
    expect(v["Series"].value, "main", "Version.Series")
    expect(v["Snapshot"].value, 0, "Version.Snapshot")

    expect(data["LevelName"].value, "objtomc world", "LevelName")
    expect(data["LastPlayed"].value, 0, "LastPlayed (deterministic)")
    expect(data["initialized"].value, 1, "initialized")

    expect(data["SpawnX"].value, 0, "SpawnX")
    expect(data["SpawnY"].value, -60, "SpawnY")
    expect(data["SpawnZ"].value, 0, "SpawnZ")
    expect(data["SpawnAngle"].value, 0.0, "SpawnAngle")

    expect(data["GameType"].value, 1, "GameType (creative)")
    expect(data["Difficulty"].value, 0, "Difficulty (peaceful)")
    expect(data["DifficultyLocked"].value, 1, "DifficultyLocked")
    expect(data["allowCommands"].value, 1, "allowCommands")
    expect(data["hardcore"].value, 0, "hardcore")
    expect(data["Time"].value, 0, "Time")
    expect(data["DayTime"].value, 6000, "DayTime (noon)")

    expect(data["raining"].value, 0, "raining")
    expect(data["rainTime"].value, 0, "rainTime")
    expect(data["thundering"].value, 0, "thundering")
    expect(data["thunderTime"].value, 0, "thunderTime")
    expect(data["clearWeatherTime"].value, 0, "clearWeatherTime")

    expect(data["BorderCenterX"].value, 0.0, "BorderCenterX")
    expect(data["BorderCenterZ"].value, 0.0, "BorderCenterZ")
    expect(data["BorderSize"].value, 60000000.0, "BorderSize")

    # ===== §3.5 implementation-time-verification additions =====
    expect(data["BorderSafeZone"].value, 5.0, "BorderSafeZone (§3.5 diff)")
    expect(data["BorderWarningBlocks"].value, 5.0, "BorderWarningBlocks (§3.5 diff)")
    expect(data["BorderWarningTime"].value, 15.0, "BorderWarningTime (§3.5 diff)")
    expect(data["BorderDamagePerBlock"].value, 0.2, "BorderDamagePerBlock (§3.5 diff)")
    expect(data["BorderSizeLerpTarget"].value, 60000000.0, "BorderSizeLerpTarget (§3.5 diff)")
    expect(data["BorderSizeLerpTime"].value, 0, "BorderSizeLerpTime (§3.5 diff)")

    expect(data["WanderingTraderSpawnChance"].value, 25, "WanderingTraderSpawnChance (§3.5 diff)")
    expect(data["WanderingTraderSpawnDelay"].value, 24000, "WanderingTraderSpawnDelay (§3.5 diff)")

    df = data["DragonFight"]
    expect(df["NeedsStateScanning"].value, 1, "DragonFight.NeedsStateScanning")
    expect(df["DragonKilled"].value, 0, "DragonFight.DragonKilled")
    expect(df["PreviouslyKilled"].value, 0, "DragonFight.PreviouslyKilled")
    if "Gateways" not in df:
        fail("DragonFight.Gateways missing")

    expect(len(list(data["CustomBossEvents"].tags)), 0, "CustomBossEvents (empty compound)")
    expect(len(data["ScheduledEvents"]), 0, "ScheduledEvents (empty list)")

    # ===== GameRules (the 6 overrides) =====
    gr = data["GameRules"]
    expect(gr["doDaylightCycle"].value, "false", "GameRules.doDaylightCycle")
    expect(gr["doWeatherCycle"].value, "false", "GameRules.doWeatherCycle")
    expect(gr["doMobSpawning"].value, "false", "GameRules.doMobSpawning")
    expect(gr["keepInventory"].value, "true", "GameRules.keepInventory")
    expect(gr["commandBlockOutput"].value, "true", "GameRules.commandBlockOutput")
    expect(gr["sendCommandFeedback"].value, "true", "GameRules.sendCommandFeedback")

    # ===== DataPacks (Enabled=[vanilla], Disabled=3 experimental) =====
    dp = data["DataPacks"]
    enabled = [t.value for t in dp["Enabled"]]
    disabled = [t.value for t in dp["Disabled"]]
    expect(enabled, ["vanilla"], "DataPacks.Enabled")
    expect(set(disabled), {"minecart_improvements", "redstone_experiments", "trade_rebalance"},
           "DataPacks.Disabled (§3.5 diff: 3 experimental packs)")

    # ===== ServerBrands =====
    expect([t.value for t in data["ServerBrands"]], ["vanilla"], "ServerBrands")
    expect(data["WasModded"].value, 0, "WasModded")

    # ===== WorldGenSettings (void preset for all 3 dimensions) =====
    wgs = data["WorldGenSettings"]
    expect(wgs["seed"].value, 0, "WorldGenSettings.seed")
    expect(wgs["generate_features"].value, 0, "WorldGenSettings.generate_features")
    expect(wgs["bonus_chest"].value, 0, "WorldGenSettings.bonus_chest")

    dims = wgs["dimensions"]
    for dim_id, biome in [
        ("minecraft:overworld",  "minecraft:plains"),
        ("minecraft:the_nether", "minecraft:nether_wastes"),
        ("minecraft:the_end",    "minecraft:the_end"),
    ]:
        if dim_id not in dims:
            fail(f"dimensions.{dim_id} missing")
        dim = dims[dim_id]
        expect(dim["type"].value, dim_id, f"dimensions.{dim_id}.type")
        gen = dim["generator"]
        expect(gen["type"].value, "minecraft:flat", f"dimensions.{dim_id}.generator.type")
        settings = gen["settings"]
        expect(settings["biome"].value, biome, f"dimensions.{dim_id}.generator.settings.biome")
        expect(settings["lakes"].value, 0, f"dimensions.{dim_id}.generator.settings.lakes")
        expect(settings["features"].value, 0, f"dimensions.{dim_id}.generator.settings.features")
        expect(len(settings["layers"]), 0,
               f"dimensions.{dim_id}.generator.settings.layers (void preset: empty list)")
        expect(len(settings["structure_overrides"]), 0,
               f"dimensions.{dim_id}.generator.settings.structure_overrides")

    print("PASS: orchestrate_level_dat_oracle")
    print(f"  verified {len(list(data.tags))} top-level Data fields + nested compounds")


if __name__ == "__main__":
    main()
