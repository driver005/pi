import os
import tempfile
import unittest

import check_style


def check(path, source, build=None):
    return [v.rule for v in check_style.FileChecker(path, source, build).run()]


class CheckStyleTest(unittest.TestCase):
    def testCleanInterface(self):
        source = """#pragma once
class IClock {
public:
    virtual ~IClock() = default;
    virtual long nowMs() const = 0;
};
"""
        self.assertEqual(check("interfaces/platform/i_clock/i_clock.h", source), [])

    def testStaticFunctionRejected(self):
        source = "class Foo {\npublic:\n    static int make();\n};\n"
        self.assertIn("no-static-function", check("src/base/foo/foo.h", source))

    def testStaticCastAndConstantsAllowed(self):
        source = """class Foo {
public:
    static constexpr int kLimit = 3;
    int get() const { return static_cast<int>(m_v); }
private:
    long m_v = 0;
};
"""
        self.assertEqual(check("src/base/foo/foo.h", source), [])

    def testAnonymousNamespaceRejected(self):
        self.assertIn("no-anonymous-namespace", check("src/base/foo/foo.cpp", "namespace {\nint a;\n}\n"))

    def testFreeFunctionRejected(self):
        self.assertIn("no-free-function", check("src/base/foo/foo.cpp", "int helper(int a) {\n    return a;\n}\n"))

    def testMemberDefinitionAllowed(self):
        source = "int Foo::get() const {\n    return m_v;\n}\nFoo::Foo(int v) : m_v{v} {\n}\n"
        self.assertEqual(check("src/base/foo/foo.cpp", source), [])

    def testMainAllowed(self):
        self.assertEqual(check("app/main.cpp", "int main(int argc, char** argv) {\n    return 0;\n}\n"), [])

    def testNestedClassRejected(self):
        source = "class Outer {\n    struct Inner {\n        int a;\n    };\n};\n"
        self.assertIn("no-nested-type", check("src/base/outer/outer.h", source))

    def testNestedEnumRejected(self):
        source = "class Outer {\n    enum class Kind { A };\n};\n"
        self.assertIn("no-nested-type", check("src/base/outer/outer.h", source))

    def testLocalStructInMethodRejected(self):
        source = "class Outer {\n    void f() {\n        struct Local { int a; };\n    }\n};\n"
        self.assertIn("no-nested-type", check("src/base/outer/outer.h", source))

    def testTwoClassesRejected(self):
        source = "class A {};\nclass B {};\n"
        self.assertIn("one-class-per-module", check("src/base/a/a.h", source))

    def testHeaderNameMismatch(self):
        self.assertIn("header-name", check("src/base/a/b.h", "class Alpha {};\n"))

    def testHeaderNameAcronym(self):
        self.assertEqual(check("src/base/http_client/http_client.h", "class HttpClient {};\n"), [])

    def testMemberPrefixRequired(self):
        source = "class A {\npublic:\n    void f();\nprivate:\n    int count;\n    int m_ok;\n};\n"
        rules = check("src/base/a/a.h", source)
        self.assertEqual(rules.count("member-prefix"), 1)

    def testAggregateStructExempt(self):
        self.assertEqual(check("interfaces/types/usage/usage.h", "struct Usage {\n    int input = 0;\n};\n"), [])

    def testAggregateWithInitializerCallsIsStillAggregate(self):
        source = "struct Doc {\n    Json header = Json::object();\n    std::string body;\n};\n"
        self.assertEqual(check("interfaces/types/doc/doc.h", source), [])

    def testStdFunctionFieldIsNotAMethod(self):
        source = "struct Req {\n    std::function<void(std::string_view)> onOutput;\n};\n"
        self.assertEqual(check("interfaces/types/req/req.h", source), [])

    def testStdFunctionMemberNeedsPrefixInClass(self):
        source = "class A {\npublic:\n    void f();\nprivate:\n    std::function<void()> cb;\n};\n"
        self.assertIn("member-prefix", check("src/base/a/a.h", source))

    def testDeletedOperatorIsNotAMember(self):
        source = "class A {\npublic:\n    A& operator=(const A&) = delete;\nprivate:\n    int m_v;\n};\n"
        self.assertEqual(check("src/base/a/a.h", source), [])

    def testStructWithMethodsNeedsPrefix(self):
        source = "struct Box {\n    int get() const { return value; }\n    int value;\n};\n"
        self.assertIn("member-prefix", check("src/base/box/box.h", source))

    def testCrossModuleIncludeRejected(self):
        source = '#include "src/base/other/other.h"\nclass A {};\n'
        self.assertIn("src-include", check("src/base/a/a.h", source))

    def testTestsMayIncludeOtherModules(self):
        source = '#include "src/base/other/other.h"\n'
        self.assertEqual(check("src/base/a/a_test.cpp", source), [])

    def testTestingSupportMayBeIncludedAnywhere(self):
        source = '#include "src/testing/fake_http_server/fake_http_server.h"\n'
        self.assertEqual(check("src/base/a/a_test.cpp", source), [])

    def testMissingBuildDepRejected(self):
        source = '#include "interfaces/types/json/json.h"\nclass A {};\n'
        self.assertIn("build-dep", check("src/base/a/a.h", source, 'deps = ["//interfaces/types/error"]'))
        self.assertNotIn("build-dep", check("src/base/a/a.h", source, 'deps = ["//interfaces/types/json"]'))

    def testOwnDirectoryIncludeNeedsNoDep(self):
        source = '#include "src/base/a/a.h"\n'
        self.assertEqual(check("src/base/a/a.cpp", source, ""), [])

    def testAppMayIncludeSrc(self):
        source = '#include "src/base/other/other.h"\n'
        self.assertEqual(check("app/application/application.cpp", source), [])

    def testCAbiExempt(self):
        source = "// style:c-abi\nextern \"C\" int f(int a);\nint g(int a) { return a; }\n"
        self.assertEqual(check("interfaces/plugin/pi_plugin/pi_plugin.h", source), [])

    def testTestFilesExempt(self):
        source = "#include <gtest/gtest.h>\nTEST(A, B) {\n    EXPECT_EQ(1, 1);\n}\n"
        self.assertEqual(check("src/base/a/a_test.cpp", source), [])

    def testTemplateBaseClass(self):
        source = "template <typename T>\nclass Box : public IBox<T> {\npublic:\n    T get() const;\nprivate:\n    T m_v;\n};\n"
        self.assertEqual(check("src/base/box/box.h", source), [])

    def testTreeWithTwoHeadersInOneDirectory(self):
        with tempfile.TemporaryDirectory() as base:
            directory = os.path.join(base, "src", "base", "a")
            os.makedirs(directory)
            for name in ("a.h", "b.h"):
                with open(os.path.join(directory, name), "w") as handle:
                    handle.write("#pragma once\n")
            rules = [v.rule for v in check_style.checkTree(base)]
            self.assertIn("one-class-per-module", rules)


if __name__ == "__main__":
    unittest.main()
