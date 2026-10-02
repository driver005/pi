"""Rules that build C++23 named modules with clang (BMI + object per module, `import std`).

Bazel's native rules do not build module interface units, so each pi module is compiled here:
  1. `clang++ --precompile -x c++-module <name>.cppm -o <module>.pcm`  (the BMI)
  2. `clang++ -c <module>.pcm -o <module>.o`                          (the object code)
Dependents receive every transitive BMI through -fmodule-file=<name>=<path>, and the objects
reach the final link through CcInfo. Third-party cc_library deps contribute include paths for the
global module fragment of the .cppm.
"""

load("@rules_cc//cc:action_names.bzl", "CPP_COMPILE_ACTION_NAME")
load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cpp_toolchain", "use_cc_toolchain")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

# BMIs record the absolute path of every input file and clang re-reads those paths whenever the
# BMI is imported. Bazel's sandbox changes that path for each action, so module actions run
# unsandboxed in the stable execroot (inputs are still declared for correct rebuilds).
_NO_SANDBOX = {"no-sandbox": "1"}

PiModuleInfo = provider(
    doc = "A compiled C++ module.",
    fields = {
        "name": "module name, e.g. pi.base.system_clock",
        "pcm": "the BMI file",
        "pcms": "depset of the BMIs of this module and everything it imports",
        "sources": "depset of the .cppm files behind those BMIs (clang re-reads them to validate)",
    },
)

def _module_flag(pcm):
    name = pcm.basename[:-len(".pcm")]
    return "-fmodule-file=%s=%s" % (name, pcm.path)

def _module_deps(deps):
    return [d[PiModuleInfo] for d in deps if PiModuleInfo in d]

def _dep_sources(deps):
    return depset(transitive = [m.sources for m in _module_deps(deps)])

def _base_flags(ctx, cc_toolchain, feature_configuration, cc_context, extra_copts):
    variables = cc_common.create_compile_variables(
        feature_configuration = feature_configuration,
        cc_toolchain = cc_toolchain,
        # Compiling a BMI to an object ignores preprocessor/library flags; do not fail on that.
        user_compile_flags = ctx.fragments.cpp.copts + ctx.fragments.cpp.cxxopts + extra_copts +
                             ["-Wno-unused-command-line-argument"],
        include_directories = cc_context.includes,
        quote_include_directories = cc_context.quote_includes,
        system_include_directories = cc_context.system_includes,
        framework_include_directories = cc_context.framework_includes,
        preprocessor_defines = cc_context.defines,
    )
    return cc_common.get_memory_inefficient_command_line(
        feature_configuration = feature_configuration,
        action_name = CPP_COMPILE_ACTION_NAME,
        variables = variables,
    )

def _setup(ctx):
    cc_toolchain = find_cpp_toolchain(ctx)
    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
        requested_features = ctx.features,
        unsupported_features = ctx.disabled_features,
    )
    compiler = cc_common.get_tool_for_action(
        feature_configuration = feature_configuration,
        action_name = CPP_COMPILE_ACTION_NAME,
    )
    env = cc_common.get_environment_variables(
        feature_configuration = feature_configuration,
        action_name = CPP_COMPILE_ACTION_NAME,
        variables = cc_common.empty_variables(),
    )
    return cc_toolchain, feature_configuration, compiler, env

def _link_info(ctx, objects, deps):
    inputs = [
        cc_common.create_linker_input(
            owner = ctx.label,
            additional_inputs = depset(objects),
            user_link_flags = [obj.path for obj in objects],
        ),
    ]
    own = CcInfo(linking_context = cc_common.create_linking_context(linker_inputs = depset(inputs)))
    return cc_common.merge_cc_infos(cc_infos = [own] + [d[CcInfo] for d in deps if CcInfo in d])

def _pi_module_impl(ctx):
    cc_toolchain, feature_configuration, compiler, env = _setup(ctx)
    deps = ctx.attr.deps + [ctx.attr._std]
    cc_context = cc_common.merge_compilation_contexts(
        compilation_contexts = [d[CcInfo].compilation_context for d in deps if CcInfo in d],
    )
    dep_pcms = depset(transitive = [m.pcms for m in _module_deps(deps)])
    dep_sources = _dep_sources(deps)
    pcm = ctx.actions.declare_file(ctx.attr.module_name + ".pcm")
    obj = ctx.actions.declare_file(ctx.attr.module_name + ".o")
    flags = _base_flags(ctx, cc_toolchain, feature_configuration, cc_context, ctx.attr.copts)
    inputs = depset([ctx.file.src], transitive = [cc_context.headers, dep_pcms, dep_sources, cc_toolchain.all_files])

    precompile = ctx.actions.args()
    precompile.add_all(flags)
    precompile.add_all(dep_pcms, map_each = _module_flag)
    precompile.add_all(["--precompile", "-x", "c++-module", ctx.file.src, "-o", pcm])
    ctx.actions.run(
        executable = compiler,
        arguments = [precompile],
        inputs = inputs,
        outputs = [pcm],
        env = env,
        mnemonic = "PiModulePrecompile",
        execution_requirements = _NO_SANDBOX,
        progress_message = "Precompiling module %s" % ctx.attr.module_name,
    )

    codegen = ctx.actions.args()
    codegen.add_all(flags)
    codegen.add_all(dep_pcms, map_each = _module_flag)
    codegen.add_all(["-c", pcm, "-o", obj])
    ctx.actions.run(
        executable = compiler,
        arguments = [codegen],
        inputs = depset([pcm, ctx.file.src], transitive = [cc_context.headers, dep_pcms, dep_sources, cc_toolchain.all_files]),
        outputs = [obj],
        env = env,
        mnemonic = "PiModuleCodegen",
        execution_requirements = _NO_SANDBOX,
        progress_message = "Compiling module %s" % ctx.attr.module_name,
    )
    return [
        DefaultInfo(files = depset([pcm, obj])),
        PiModuleInfo(
            name = ctx.attr.module_name,
            pcm = pcm,
            pcms = depset([pcm], transitive = [dep_pcms]),
            sources = depset([ctx.file.src], transitive = [dep_sources]),
        ),
        _link_info(ctx, [obj], deps),
    ]

pi_cc_module = rule(
    implementation = _pi_module_impl,
    attrs = {
        "src": attr.label(allow_single_file = [".cppm"], mandatory = True),
        "module_name": attr.string(mandatory = True),
        "deps": attr.label_list(),
        "copts": attr.string_list(),
        "_std": attr.label(default = Label("//tools:std_module")),
        "_cc_toolchain": attr.label(default = Label("@rules_cc//cc:current_cc_toolchain")),
    },
    fragments = ["cpp"],
    toolchains = use_cc_toolchain(),
)

def _pi_tu_impl(ctx):
    cc_toolchain, feature_configuration, compiler, env = _setup(ctx)
    deps = ctx.attr.deps + [ctx.attr._std]
    cc_context = cc_common.merge_compilation_contexts(
        compilation_contexts = [d[CcInfo].compilation_context for d in deps if CcInfo in d],
    )
    dep_pcms = depset(transitive = [m.pcms for m in _module_deps(deps)])
    dep_sources = _dep_sources(deps)
    flags = _base_flags(ctx, cc_toolchain, feature_configuration, cc_context, ctx.attr.copts)
    args = ctx.actions.args()
    args.add_all(flags)
    args.add_all(dep_pcms, map_each = _module_flag)
    args.add_all(["-c", ctx.file.src, "-o", ctx.outputs.obj])
    ctx.actions.run(
        executable = compiler,
        arguments = [args],
        inputs = depset([ctx.file.src], transitive = [cc_context.headers, dep_pcms, dep_sources, cc_toolchain.all_files]),
        outputs = [ctx.outputs.obj],
        env = env,
        mnemonic = "PiModuleUserCompile",
        execution_requirements = _NO_SANDBOX,
        progress_message = "Compiling %s" % ctx.file.src.short_path,
    )
    return [DefaultInfo(files = depset([ctx.outputs.obj]))]

pi_cc_tu = rule(
    doc = "Compiles an ordinary translation unit (test or main) that imports pi modules.",
    implementation = _pi_tu_impl,
    attrs = {
        "src": attr.label(allow_single_file = [".cpp"], mandatory = True),
        "deps": attr.label_list(),
        "copts": attr.string_list(),
        "_std": attr.label(default = Label("//tools:std_module")),
        "_cc_toolchain": attr.label(default = Label("@rules_cc//cc:current_cc_toolchain")),
    },
    outputs = {"obj": "%{name}.o"},
    fragments = ["cpp"],
    toolchains = use_cc_toolchain(),
)

def _pi_std_impl(ctx):
    cc_toolchain, feature_configuration, compiler, env = _setup(ctx)
    cc_context = cc_common.create_compilation_context()
    flags = _base_flags(ctx, cc_toolchain, feature_configuration, cc_context, ctx.attr.copts)
    pcm = ctx.actions.declare_file("std.pcm")
    obj = ctx.actions.declare_file("std.o")
    args = ctx.actions.args()
    args.add(compiler)
    args.add_all(flags)
    args.add_all(["--precompile", "-x", "c++-module", "-Wno-reserved-module-identifier", "-o", pcm])
    ctx.actions.run_shell(
        command = 'cxx="$1"; shift; src="$(dirname "$(readlink -f "$(command -v "$cxx")")")/../share/libc++/v1/std.cppm"; ' +
                  '[ -f "$src" ] || { echo "libc++ std.cppm not found next to $cxx" >&2; exit 1; }; ' +
                  'exec "$cxx" "$@" "$src"',
        arguments = [args],
        inputs = cc_toolchain.all_files,
        outputs = [pcm],
        env = env,
        mnemonic = "PiStdPrecompile",
        execution_requirements = _NO_SANDBOX,
        progress_message = "Precompiling the std module",
        use_default_shell_env = True,
    )
    codegen = ctx.actions.args()
    codegen.add_all(flags)
    codegen.add_all(["-Wno-reserved-module-identifier", "-c", pcm, "-o", obj])
    ctx.actions.run(
        executable = compiler,
        arguments = [codegen],
        inputs = depset([pcm], transitive = [cc_toolchain.all_files]),
        outputs = [obj],
        env = env,
        mnemonic = "PiStdCodegen",
        execution_requirements = _NO_SANDBOX,
        progress_message = "Compiling the std module",
    )
    link = _link_info(ctx, [obj], [])
    return [
        DefaultInfo(files = depset([pcm, obj])),
        PiModuleInfo(name = "std", pcm = pcm, pcms = depset([pcm]), sources = depset()),
        link,
    ]

pi_std_module = rule(
    doc = "Builds libc++'s `std` module (import std).",
    implementation = _pi_std_impl,
    attrs = {
        "copts": attr.string_list(),
        "_cc_toolchain": attr.label(default = Label("@rules_cc//cc:current_cc_toolchain")),
    },
    fragments = ["cpp"],
    toolchains = use_cc_toolchain(),
)
