import os
import tempfile
import unittest

import check_style


def check(path, source, build=None):
    return [v.rule for v in check_style.FileChecker(path, source, build).run()]


class CheckStyleTest(unittest.TestCase):
    def testCleanInterface(self):
        source = """export module pi.platform.i_clock;
import std;
export class IClock {
public:
    virtual ~IClock() = default;
    virtual long nowMs() const = 0;
};
"""
        self.assertEqual(check("interfaces/platform/i_clock/i_clock.cppm", source), [])

    def testStaticFunctionRejected(self):
        source = "export class Foo {\npublic:\n    static int make();\n};\n"
        self.assertIn("no-static-function", check("src/base/foo/foo.cppm", source))

    def testStaticCastAndConstantsAllowed(self):
        source = """export class Foo {
public:
    static constexpr int kLimit = 3;
    int get() const { return static_cast<int>(m_v); }
private:
    long m_v = 0;
};
"""
        self.assertEqual(check("src/base/foo/foo.cppm", source), [])

    def testAnonymousNamespaceRejected(self):
        self.assertIn("no-anonymous-namespace", check("src/base/foo/foo.cppm", "namespace {\nint a;\n}\n"))

    def testFreeFunctionRejected(self):
        self.assertIn("no-free-function", check("src/base/foo/foo.cppm", "int helper(int a) {\n    return a;\n}\n"))

    def testMemberDefinitionAllowed(self):
        source = "int Foo::get() const {\n    return m_v;\n}\nFoo::Foo(int v) : m_v{v} {\n}\n"
        self.assertEqual(check("src/base/foo/foo.cppm", source), [])

    def testMainAllowed(self):
        self.assertEqual(check("app/main.cpp", "int main(int argc, char** argv) {\n    return 0;\n}\n"), [])

    def testNestedClassRejected(self):
        source = "export class Outer {\n    struct Inner {\n        int a;\n    };\n};\n"
        self.assertIn("no-nested-type", check("src/base/outer/outer.cppm", source))

    def testNestedEnumRejected(self):
        source = "export class Outer {\n    enum class Kind { A };\n};\n"
        self.assertIn("no-nested-type", check("src/base/outer/outer.cppm", source))

    def testLocalStructInMethodRejected(self):
        source = "export class Outer {\n    void f() {\n        struct Local { int a; };\n    }\n};\n"
        self.assertIn("no-nested-type", check("src/base/outer/outer.cppm", source))

    def testTwoClassesRejected(self):
        source = "export class A {};\nexport class B {};\n"
        self.assertIn("one-class-per-module", check("src/base/a/a.cppm", source))

    def testModuleNameMismatch(self):
        self.assertIn("module-name", check("src/base/a/b.cppm", "export class Alpha {};\n"))

    def testModuleNameAcronym(self):
        self.assertEqual(check("src/base/http_client/http_client.cppm", "export class HttpClient {};\n"), [])

    def testMemberPrefixRequired(self):
        source = "export class A {\npublic:\n    void f();\nprivate:\n    int count;\n    int m_ok;\n};\n"
        rules = check("src/base/a/a.cppm", source)
        self.assertEqual(rules.count("member-prefix"), 1)

    def testAggregateStructExempt(self):
        self.assertEqual(check("interfaces/types/usage/usage.cppm", "export struct Usage {\n    int input = 0;\n};\n"), [])

    def testAggregateWithInitializerCallsIsStillAggregate(self):
        source = "export struct Doc {\n    Json header = Json::object();\n    std::string body;\n};\n"
        self.assertEqual(check("interfaces/types/doc/doc.cppm", source), [])

    def testStdFunctionFieldIsNotAMethod(self):
        source = "export struct Req {\n    std::function<void(std::string_view)> onOutput;\n};\n"
        self.assertEqual(check("interfaces/types/req/req.cppm", source), [])

    def testStdFunctionMemberNeedsPrefixInClass(self):
        source = "export class A {\npublic:\n    void f();\nprivate:\n    std::function<void()> cb;\n};\n"
        self.assertIn("member-prefix", check("src/base/a/a.cppm", source))

    def testDeletedOperatorIsNotAMember(self):
        source = "export class A {\npublic:\n    A& operator=(const A&) = delete;\nprivate:\n    int m_v;\n};\n"
        self.assertEqual(check("src/base/a/a.cppm", source), [])

    def testStructWithMethodsNeedsPrefix(self):
        source = "export struct Box {\n    int get() const { return value; }\n    int value;\n};\n"
        self.assertIn("member-prefix", check("src/base/box/box.cppm", source))

    def testMissingBuildDepRejected(self):
        source = "export module pi.base.a;\nimport pi.types.json;\nexport class A {};\n"
        self.assertIn("build-dep", check("src/base/a/a.cppm", source, 'deps = ["//interfaces/types/error"]'))
        self.assertNotIn("build-dep", check("src/base/a/a.cppm", source, 'deps = ["//interfaces/types/json"]'))

    def testExportImportCountsAsImport(self):
        source = "export module pi.base.a;\nexport import pi.types.json;\n"
        self.assertIn("build-dep", check("src/base/a/a.cppm", source, ""))

    def testStdImportNeedsNoDep(self):
        source = "export module pi.base.a;\nimport std;\n"
        self.assertEqual(check("src/base/a/a.cppm", source, ""), [])

    def testProductionModuleMayNotImportSrcModule(self):
        source = "export module pi.base.a;\nimport pi.base.other;\n"
        self.assertIn("src-import", check("src/base/a/a.cppm", source, 'deps = ["//src/base/other"]'))

    def testTestsAppAndTestingMayImportSrcModules(self):
        source = "import pi.base.other;\n"
        build = 'deps = ["//src/base/other"]'
        self.assertEqual(check("src/base/a/a_test.cpp", source, build), [])
        self.assertEqual(check("app/application/application.cpp", source, build), [])
        self.assertEqual(check("src/testing/fake/fake.cppm", source, build), [])

    def testCxxStdIncludeRejectedButCHeadersAllowed(self):
        self.assertIn("std-include", check("src/base/a/a.cppm", "module;\n#include <vector>\nexport module pi.base.a;\n"))
        self.assertEqual(check("src/base/a/a.cppm", "module;\n#include <unistd.h>\n#include <cstdio>\nexport module pi.base.a;\n"), [])

    def testCAbiExempt(self):
        source = "// style:c-abi\nextern \"C\" int f(int a);\nint g(int a) { return a; }\n"
        self.assertEqual(check("interfaces/plugin/pi_plugin/pi_plugin.cppm", source), [])

    def testTestFilesExempt(self):
        source = "#include <gtest/gtest.h>\nTEST(A, B) {\n    EXPECT_EQ(1, 1);\n}\n"
        self.assertEqual(check("src/base/a/a_test.cpp", source), [])

    def testTemplateClass(self):
        source = "export template <typename T>\nclass Box : public IBox<T> {\npublic:\n    T get() const;\nprivate:\n    T m_v;\n};\n"
        self.assertEqual(check("src/base/box/box.cppm", source), [])

    def testTreeWithTwoModulesInOneDirectory(self):
        with tempfile.TemporaryDirectory() as base:
            directory = os.path.join(base, "src", "base", "a")
            os.makedirs(directory)
            for name in ("a.cppm", "b.cppm"):
                with open(os.path.join(directory, name), "w") as handle:
                    handle.write("export module pi.base.a;\n")
            rules = [v.rule for v in check_style.checkTree(base)]
            self.assertIn("one-class-per-module", rules)


if __name__ == "__main__":
    unittest.main()
