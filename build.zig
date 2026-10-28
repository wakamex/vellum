// vellum on gesso, which it expects at ../gesso.
//   zig build                 zig-out/bin/vellum
//   zig build -Ddev           one that starts again as each new build (tools/dev.sh)
//   zig build test            the parser's tests
const std = @import("std");

const flags: []const []const u8 = &.{ "-std=c11", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-missing-field-initializers" };
const vendor_flags: []const []const u8 = &.{ "-std=c11", "-w", "-fno-sanitize=undefined" };

fn parser(b: *std.Build, mod: *std.Build.Module) void {
    mod.addIncludePath(b.path("src"));
    mod.addIncludePath(b.path("vendor/md4c"));
    mod.addCSourceFiles(.{ .root = b.path("src"), .files = &.{"doc.c"}, .flags = flags });
    mod.addCSourceFiles(.{ .root = b.path("vendor/md4c"), .files = &.{ "md4c.c", "entity.c" }, .flags = vendor_flags });
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const dev = b.option(bool, "dev", "A dev build: starts again, where it was, when its program file is replaced") orelse false;
    const gesso = b.dependency("gesso", .{ .target = target, .optimize = optimize }).artifact("gesso");

    const mod = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true, .strip = optimize != .debug });
    parser(b, mod);
    mod.addCSourceFiles(.{ .root = b.path("src"), .files = &.{"main.c"}, .flags = flags });
    if (dev) mod.addCMacro("VELLUM_DEV", "1");
    mod.linkLibrary(gesso);
    const exe = b.addExecutable(.{ .name = "vellum", .root_module = mod });
    // A window program on Windows: no console comes up with it. Its icon (assets/icon/update.sh makes
    // it) is resource 1, which Explorer shows and the window takes.
    if (target.result.os.tag == .windows) {
        exe.subsystem = .windows;
        mod.addWin32ResourceFile(.{ .file = b.path("assets/icon/vellum.rc") });
    }
    b.installArtifact(exe);

    const tests = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true });
    parser(b, tests);
    tests.addCSourceFiles(.{ .root = b.path("tests"), .files = &.{"test.c"}, .flags = flags });
    const run = b.addRunArtifact(b.addExecutable(.{ .name = "vellum-tests", .root_module = tests }));
    run.setCwd(b.path("."));
    b.step("test", "Run the tests").dependOn(&run.step);
}
