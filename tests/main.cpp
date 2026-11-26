#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>

#include "coop/net/bind_guard.hpp"

// The tests are written in English: the module's own text is what they compare
// (a row's name, a help line, a drawn caption), and Tr() leaves English as it
// is when the language has no table. ReadLanguage() reads a language.txt
// beside the program when there is one, so a machine with a Chinese one there
// would translate every one of those strings and the asserts would read
// "房主" where they expect "Host". HOS_LANGUAGE is the first thing
// ReadLanguage() looks at, so pinning it here keeps the module's text English
// on any machine -- the i18n tests set the language they want by passing one
// to Tr(lang, ...) themselves.
namespace {
struct EnglishTests {
    EnglishTests() {
#ifdef _WIN32
        _putenv_s("HOS_LANGUAGE", "en");
#else
        setenv("HOS_LANGUAGE", "en", 0);   // (not overriding a language a test set)
#endif
    }
} g_english_tests;
}  // namespace

// No test may listen on all addresses: Windows Defender Firewall asks the person about every program path that does,
// and every build folder has its own test programs (dozens of rules). Every listener of the server library takes the
// address to bind (coop/net/bind_guard.hpp); with the guard on, one that is not loopback is refused and counted, and
// the program fails here even when the test that opened it did not look at the result.
int main(int argc, char** argv) {
    coop::net::require_loopback_binds(true);
    coop::net::set_test_bind(coop::net::kLoopbackIp);  // (code that has no config for it: the Hookshot menu)
    doctest::Context context(argc, argv);
    int result = context.run();
    if (context.shouldExit()) return result;  // (--help, --list-test-cases ...)
    if (const int refused = coop::net::refused_binds(); refused != 0) {
        std::fprintf(stderr,
                     "FAILED: %d socket(s) tried to bind to an address other than 127.0.0.1 (the Windows firewall would ask "
                     "about this program). Give the listener a bind address in the test (docs/testing.md, 'Windows firewall').\n",
                     refused);
        result = result != 0 ? result : 1;
    }
    return result;
}
