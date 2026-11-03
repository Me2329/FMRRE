#pragma once
#include <string>
#include <utility>
#include <vector>

namespace tw::i18n {

// UI languages. Translation happens in one place - the font layer looks every string up just
// before measuring or drawing it - so screens keep writing plain English. Unknown strings
// (file names, partitions, log lines, anything not in the dictionary) stay as they are.
// Entries with {} also match strings built at run time ("Swipe to install to slot {}").

struct Language { const char* code; const char* name; };
std::vector<Language> languages();     // {"en", "English"}, {"ro", "Română"}

void setLanguage(const std::string& code);
const std::string& language();

// Translated text (the same reference when there is no translation). A pattern result stays valid
// for the next 63 pattern translations on this thread: copy it if you keep it.
const std::string& tr(const std::string& s);

// While one of these lives on a thread, nothing is translated there: for user content such as
// file names, typed text, terminal output and logs.
struct NoTranslate {
    NoTranslate();
    ~NoTranslate();
};

// Upper case for UTF-8 text: ASCII, Latin-1, Latin Extended-A and the Romanian ș ț.
std::string upper(const std::string& s);

// "Tuesday, 03 November" in the current language.
std::string longDate(int weekday, int day, int month);

// For the self-tests: every dictionary entry of a language.
std::vector<std::pair<std::string, std::string>> entries(const std::string& code);

}  // namespace tw::i18n
