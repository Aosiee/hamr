#pragma once

// hamr/test.h - hamr's C++ test library. One header, C++17, the standard
// library only.
//
//     #define HAMR_TEST_MAIN          // in one translation unit, for the default main
//     #include "hamr/test.h"
//
//     TEST_CASE("a vector grows" * hamr::test::timeout(5)) {
//         std::vector<int> v;
//         v.push_back(3);
//         REQUIRE(v.size() == 1);
//         CHECK(v[0] == 3);
//         CHECK(0.1 + 0.2 == hamr::test::Approx(0.3));
//     }
//
// A test binary runs on its own (console output, `--case <glob>` filters,
// `--list`), and under hamr, which lists its cases (`--list-json <file>`),
// runs them in batches side by side (`--run <ids file> --events <file>`),
// and reads what each case did from the events file: one JSON object per
// line, flushed as it is written, so a crash leaves every finished case's
// result behind it.
//
// A custom main calls hamr::test::Run(argc, argv) and returns its result.
//
// Checks may run on any thread (a case's worker threads may CHECK). INFO
// and CAPTURE belong to the thread that wrote them. A failed REQUIRE ends
// its case by an exception, so it belongs on the case's own thread; on
// another thread, use CHECK.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
#define HAMR_TEST_EXCEPTIONS 1
#else
#define HAMR_TEST_EXCEPTIONS 0
#endif

namespace hamr::test {

// ---- decorators -------------------------------------------------------------

// The seconds a case may take before hamr ends it (0: the suite's default).
struct timeout {
	double Seconds;
	explicit timeout(double seconds) : Seconds(seconds) {}
};

// A case that is listed but not run (unless `--no-skip`).
struct skip {
	bool Value;
	explicit skip(bool value = true) : Value(value) {}
};

// Words a suite can select cases by (comma separated).
struct tags {
	std::string Value;
	explicit tags(std::string value) : Value(std::move(value)) {}
};

struct CaseSpec {
	std::string Name;
	double Timeout = 0;
	bool Skip = false;
	std::string Tags;
	CaseSpec(const char* name) : Name(name) {}
	CaseSpec(std::string name) : Name(std::move(name)) {}
};

inline CaseSpec operator*(CaseSpec spec, const timeout& t) { spec.Timeout = t.Seconds; return spec; }
inline CaseSpec operator*(CaseSpec spec, const skip& s) { spec.Skip = s.Value; return spec; }
inline CaseSpec operator*(CaseSpec spec, const tags& t) { spec.Tags = t.Value; return spec; }
inline CaseSpec operator*(const char* name, const timeout& t) { return CaseSpec(name) * t; }
inline CaseSpec operator*(const char* name, const skip& s) { return CaseSpec(name) * s; }
inline CaseSpec operator*(const char* name, const tags& t) { return CaseSpec(name) * t; }

// ---- Approx -----------------------------------------------------------------

// Equality within a tolerance: |a - b| < epsilon * (scale + max(|a|, |b|)).
class Approx {
public:
	explicit Approx(double value) : m_Value(value) {}
	Approx operator()(double value) const {
		Approx a(value);
		a.m_Epsilon = m_Epsilon;
		a.m_Scale = m_Scale;
		return a;
	}
	Approx& epsilon(double e) { m_Epsilon = e; return *this; }
	Approx& scale(double s) { m_Scale = s; return *this; }
	bool Equals(double other) const {
		return std::fabs(other - m_Value) < m_Epsilon * (m_Scale + std::max(std::fabs(other), std::fabs(m_Value)));
	}
	double Value() const { return m_Value; }
	friend std::ostream& operator<<(std::ostream& out, const Approx& a) { return out << "Approx( " << a.m_Value << " )"; }

private:
	double m_Value;
	double m_Epsilon = static_cast<double>(std::numeric_limits<float>::epsilon()) * 100;
	double m_Scale = 1.0;
};

template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
bool operator==(const T& lhs, const Approx& rhs) { return rhs.Equals(static_cast<double>(lhs)); }
template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
bool operator==(const Approx& lhs, const T& rhs) { return lhs.Equals(static_cast<double>(rhs)); }
template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
bool operator!=(const T& lhs, const Approx& rhs) { return !rhs.Equals(static_cast<double>(lhs)); }
template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
bool operator!=(const Approx& lhs, const T& rhs) { return !lhs.Equals(static_cast<double>(rhs)); }
template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
bool operator<=(const T& lhs, const Approx& rhs) { return static_cast<double>(lhs) < rhs.Value() || rhs.Equals(static_cast<double>(lhs)); }
template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
bool operator<=(const Approx& lhs, const T& rhs) { return lhs.Value() < static_cast<double>(rhs) || lhs.Equals(static_cast<double>(rhs)); }
template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
bool operator>=(const T& lhs, const Approx& rhs) { return static_cast<double>(lhs) > rhs.Value() || rhs.Equals(static_cast<double>(lhs)); }
template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
bool operator>=(const Approx& lhs, const T& rhs) { return lhs.Value() > static_cast<double>(rhs) || lhs.Equals(static_cast<double>(rhs)); }
template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
bool operator<(const T& lhs, const Approx& rhs) { return static_cast<double>(lhs) < rhs.Value() && !rhs.Equals(static_cast<double>(lhs)); }
template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
bool operator<(const Approx& lhs, const T& rhs) { return lhs.Value() < static_cast<double>(rhs) && !lhs.Equals(static_cast<double>(rhs)); }
template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
bool operator>(const T& lhs, const Approx& rhs) { return static_cast<double>(lhs) > rhs.Value() && !rhs.Equals(static_cast<double>(lhs)); }
template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
bool operator>(const Approx& lhs, const T& rhs) { return lhs.Value() > static_cast<double>(rhs) && !lhs.Equals(static_cast<double>(rhs)); }

// ---- turning values into text -------------------------------------------------

namespace detail {

template <class T, class = void>
struct Streamable : std::false_type {};
template <class T>
struct Streamable<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>> : std::true_type {};

inline std::string Quote(std::string_view s) {
	std::string out = "\"";
	out.append(s.data(), s.size());
	out += '"';
	return out;
}

} // namespace detail

// How a value reads in a failure ("{?}" when it has no operator<<).
template <class T>
std::string ToString(const T& value) {
	using U = std::decay_t<T>;
	if constexpr (std::is_same_v<U, bool>) {
		return value ? "true" : "false";
	} else if constexpr (std::is_same_v<U, std::nullptr_t>) {
		return "nullptr";
	} else if constexpr (std::is_same_v<U, char>) {
		// A printable char reads as itself and its code, anything else as its code.
		const int code = static_cast<unsigned char>(value);
		if (code >= 0x20 && code < 0x7f)
			return std::string("'") + value + "' (" + std::to_string(code) + ")";
		return std::to_string(code);
	} else if constexpr (std::is_same_v<U, signed char> || std::is_same_v<U, unsigned char>) {
		// Byte-sized integers (int8_t, uint8_t): their value.
		return std::to_string(static_cast<int>(value));
	} else if constexpr (std::is_same_v<U, std::string> || std::is_same_v<U, std::string_view>) {
		return detail::Quote(value);
	} else if constexpr (std::is_same_v<U, const char*> || std::is_same_v<U, char*> || std::is_array_v<T>) {
		if constexpr (std::is_convertible_v<const T&, const char*>) {
			const char* s = value;
			return s ? detail::Quote(s) : std::string("nullptr");
		} else {
			return "{?}";
		}
	} else if constexpr (std::is_floating_point_v<U>) {
		std::ostringstream out;
		out.precision(std::numeric_limits<U>::max_digits10);
		out << value;
		return out.str();
	} else if constexpr (detail::Streamable<U>::value) {
		std::ostringstream out;
		out << value;
		return out.str();
	} else if constexpr (std::is_enum_v<U>) {
		return std::to_string(static_cast<long long>(value));
	} else if constexpr (std::is_pointer_v<U>) {
		if (value == nullptr)
			return "nullptr";
		std::ostringstream out;
		out << static_cast<const void*>(value);
		return out.str();
	} else {
		return "{?}";
	}
}

// ---- the registry -------------------------------------------------------------

using CaseFn = void (*)();

struct Case {
	std::string Name;
	const char* File;
	int Line;
	double Timeout;
	bool Skip;
	std::string Tags;
	CaseFn Fn;
};

namespace detail {

inline std::vector<Case>& Registry() {
	static std::vector<Case> cases;
	return cases;
}

struct Registrar {
	Registrar(const CaseSpec& spec, const char* file, int line, CaseFn fn) {
		// A header of cases included by two translation units registers once.
		for (const Case& c : Registry())
			if (c.Line == line && std::strcmp(c.File, file) == 0)
				return;
		Registry().push_back({spec.Name, file, line, spec.Timeout, spec.Skip, spec.Tags, fn});
	}
};

// ---- what a run records ---------------------------------------------------------

struct Failure {
	std::string Kind; // check, require, warn, fail, exception
	std::string File;
	int Line = 0;
	std::string Expression;
	std::string Expanded;
	std::string Message;
	std::vector<std::string> Context;
	std::string Subcase;
};

struct Subcases {
	std::set<std::string> Done;
	std::vector<std::string> Stack;
	std::vector<bool> EnteredAt;
	std::vector<bool> PendingAt;
	bool Incomplete = false;
};

struct State {
	// Guards Failures, the events file and the console: checks may come
	// from a case's worker threads.
	std::mutex Lock;
	std::vector<Failure> Failures;
	std::atomic<int> Assertions{0};
	Subcases Sub;
	const Case* Current = nullptr;
	std::FILE* Events = nullptr;
	bool Console = true;
};

inline State& St() {
	static State state;
	return state;
}

// INFO and CAPTURE context: the writing thread's own.
inline std::vector<std::function<std::string()>>& Context() {
	thread_local std::vector<std::function<std::string()>> context;
	return context;
}

// The exception a failed REQUIRE ends its case with.
struct RequireAbort {};

inline std::string JsonString(std::string_view s) {
	std::string out = "\"";
	for (unsigned char c : s) {
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20) {
				char buffer[8];
				std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
				out += buffer;
			} else {
				out += static_cast<char>(c);
			}
		}
	}
	out += '"';
	return out;
}

// Writes one events line; the caller holds the lock (or is the only thread).
inline void Emit(const std::string& line) {
	if (St().Events) {
		std::fputs(line.c_str(), St().Events);
		std::fputc('\n', St().Events);
		std::fflush(St().Events);
	}
}

inline std::string SubcasePath() {
	std::string out;
	for (const std::string& s : St().Sub.Stack)
		out += (out.empty() ? "" : " / ") + s;
	return out;
}

class MessageBuilder {
public:
	template <class T>
	MessageBuilder& operator<<(const T& value) {
		if constexpr (std::is_convertible_v<const T&, std::string_view> || Streamable<T>::value)
			m_Out << value;
		else
			m_Out << ToString(value);
		return *this;
	}
	std::string Str() const { return m_Out.str(); }

private:
	std::ostringstream m_Out;
};

inline void Record(Failure failure) {
	State& s = St();
	for (auto& context : Context())
		failure.Context.push_back(context());
	std::lock_guard<std::mutex> lock(s.Lock);
	failure.Subcase = SubcasePath();
	if (s.Console) {
		std::fprintf(stderr, "%s:%d: %s in \"%s\"%s%s\n", failure.File.c_str(), failure.Line,
			failure.Kind == "warn" ? "WARNING" : "FAILED", s.Current ? s.Current->Name.c_str() : "?",
			failure.Subcase.empty() ? "" : " / ", failure.Subcase.c_str());
		if (!failure.Expression.empty())
			std::fprintf(stderr, "  %s\n", failure.Expression.c_str());
		if (!failure.Expanded.empty())
			std::fprintf(stderr, "    with values: %s\n", failure.Expanded.c_str());
		if (!failure.Message.empty())
			std::fprintf(stderr, "    %s\n", failure.Message.c_str());
		for (const std::string& c : failure.Context)
			std::fprintf(stderr, "    context: %s\n", c.c_str());
	}
	std::string context = "[";
	for (size_t i = 0; i < failure.Context.size(); i++)
		context += (i ? "," : "") + JsonString(failure.Context[i]);
	context += "]";
	Emit("{\"event\":\"failure\",\"kind\":" + JsonString(failure.Kind) + ",\"file\":" + JsonString(failure.File) +
		",\"line\":" + std::to_string(failure.Line) + ",\"expression\":" + JsonString(failure.Expression) +
		",\"expanded\":" + JsonString(failure.Expanded) + ",\"message\":" + JsonString(failure.Message) +
		",\"context\":" + context + ",\"subcase\":" + JsonString(failure.Subcase) + "}");
	if (failure.Kind != "warn")
		s.Failures.push_back(std::move(failure));
}

[[noreturn]] inline void AbortCase() {
#if HAMR_TEST_EXCEPTIONS
	throw RequireAbort{};
#else
	// Without exceptions a failed REQUIRE cannot unwind its case: the case
	// is reported as failed and the process ends; hamr runs the rest of
	// the batch in a new one.
	Emit("{\"event\":\"case-end\",\"status\":\"fail\",\"aborted\":true}");
	std::fflush(nullptr);
	std::_Exit(3);
#endif
}

// ---- expression decomposition ----------------------------------------------------

struct Result {
	bool Passed;
	std::string Expanded;
};

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wfloat-equal"
#endif

template <class L>
struct Lhs {
	const L& Value;
	operator Result() const { return {static_cast<bool>(Value), ToString(Value)}; }
	template <class R> Result operator==(const R& rhs) const { return {static_cast<bool>(Value == rhs), ToString(Value) + " == " + ToString(rhs)}; }
	template <class R> Result operator!=(const R& rhs) const { return {static_cast<bool>(Value != rhs), ToString(Value) + " != " + ToString(rhs)}; }
	template <class R> Result operator<(const R& rhs) const { return {static_cast<bool>(Value < rhs), ToString(Value) + " < " + ToString(rhs)}; }
	template <class R> Result operator<=(const R& rhs) const { return {static_cast<bool>(Value <= rhs), ToString(Value) + " <= " + ToString(rhs)}; }
	template <class R> Result operator>(const R& rhs) const { return {static_cast<bool>(Value > rhs), ToString(Value) + " > " + ToString(rhs)}; }
	template <class R> Result operator>=(const R& rhs) const { return {static_cast<bool>(Value >= rhs), ToString(Value) + " >= " + ToString(rhs)}; }
};

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

struct Decomposer {
	template <class L>
	Lhs<L> operator<=(const L& value) const { return {value}; }
};

inline void Assert(const char* macro, bool require, bool negate, const Result& r, const char* expr, const char* file, int line,
	const std::string& message, bool warn = false) {
	St().Assertions++;
	if (r.Passed != negate)
		return;
	Failure f;
	f.Kind = warn ? "warn" : require ? "require" : "check";
	f.File = file;
	f.Line = line;
	f.Expression = std::string(macro) + "( " + expr + " )";
	f.Expanded = r.Expanded;
	f.Message = message;
	Record(std::move(f));
	if (require)
		AbortCase();
}

inline void Throws(const char* macro, bool require, bool threw, bool expected, const char* expr, const char* file, int line,
	const std::string& note) {
	St().Assertions++;
	if (threw == expected)
		return;
	Failure f;
	f.Kind = require ? "require" : "check";
	f.File = file;
	f.Line = line;
	f.Expression = std::string(macro) + "( " + expr + " )";
	f.Message = note;
	Record(std::move(f));
	if (require)
		AbortCase();
}

inline void Message(const char* file, int line, const std::string& text) {
	State& s = St();
	std::lock_guard<std::mutex> lock(s.Lock);
	if (s.Console)
		std::fprintf(stderr, "%s:%d: MESSAGE: %s\n", file, line, text.c_str());
	Emit("{\"event\":\"message\",\"file\":" + JsonString(file) + ",\"line\":" + std::to_string(line) +
		",\"text\":" + JsonString(text) + "}");
}

inline void Fail(const char* file, int line, const std::string& text) {
	Failure f;
	f.Kind = "fail";
	f.File = file;
	f.Line = line;
	f.Message = text;
	Record(std::move(f));
}

class ScopedInfo {
public:
	explicit ScopedInfo(std::function<std::string()> text) { Context().push_back(std::move(text)); }
	~ScopedInfo() { Context().pop_back(); }
	ScopedInfo(const ScopedInfo&) = delete;
	ScopedInfo& operator=(const ScopedInfo&) = delete;
};

// A SUBCASE: the case runs again for each leaf, entering one new path a run.
class Subcase {
public:
	Subcase(const char* name) {
		Subcases& s = St().Sub;
		size_t depth = s.Stack.size();
		std::string key;
		for (const std::string& part : s.Stack)
			key += part + "\x1f";
		key += name;
		if (s.EnteredAt.size() <= depth)
			s.EnteredAt.resize(depth + 1, false);
		if (s.PendingAt.size() <= depth + 1)
			s.PendingAt.resize(depth + 2, false);
		if (s.Done.count(key))
			return;
		if (s.EnteredAt[depth]) {
			s.PendingAt[depth] = true; // a sibling ran this time: this one runs later
			return;
		}
		s.EnteredAt[depth] = true;
		s.Stack.push_back(name);
		m_Key = key;
		m_Entered = true;
	}
	~Subcase() {
		if (!m_Entered)
			return;
		Subcases& s = St().Sub;
		size_t depth = s.Stack.size(); // children are at this depth
		bool childrenPending = depth < s.PendingAt.size() && s.PendingAt[depth];
		if (!childrenPending)
			s.Done.insert(m_Key);
		else
			s.Incomplete = true;
		if (depth < s.PendingAt.size())
			s.PendingAt[depth] = false;
		if (s.EnteredAt.size() > depth)
			s.EnteredAt.resize(depth);
		s.Stack.pop_back();
	}
	explicit operator bool() const { return m_Entered; }

private:
	std::string m_Key;
	bool m_Entered = false;
};

inline bool Match(const char* p, const char* s) {
	while (*p) {
		if (*p == '*') {
			p++;
			for (const char* t = s;; t++) {
				if (Match(p, t))
					return true;
				if (!*t)
					return false;
			}
		}
		if (!*s || (*p != '?' && *p != *s))
			return false;
		p++;
		s++;
	}
	return !*s;
}

struct CaseResult {
	std::string Status;
	double Seconds = 0;
	int Assertions = 0;
	size_t Failures = 0;
};

// A guard against subcases that never settle (a name built from a counter).
inline constexpr int kMaxCaseRuns = 100000;

inline CaseResult RunCase(const Case& c) {
	State& s = St();
	{
		std::lock_guard<std::mutex> lock(s.Lock);
		s.Current = &c;
		s.Failures.clear();
		Emit("{\"event\":\"case-start\",\"name\":" + JsonString(c.Name) + "}");
	}
	s.Assertions = 0;
	Context().clear();
	s.Sub = Subcases{};
	const auto start = std::chrono::steady_clock::now();
	for (int run = 0; run < kMaxCaseRuns; run++) {
		s.Sub.Stack.clear();
		s.Sub.EnteredAt.clear();
		s.Sub.Incomplete = false;
		std::fill(s.Sub.PendingAt.begin(), s.Sub.PendingAt.end(), false);
#if HAMR_TEST_EXCEPTIONS
		try {
			c.Fn();
		} catch (const RequireAbort&) {
		} catch (const std::exception& e) {
			Failure f;
			f.Kind = "exception";
			f.File = c.File;
			f.Line = c.Line;
			f.Message = std::string("an exception escaped the case: ") + e.what();
			Record(std::move(f));
		} catch (...) {
			Failure f;
			f.Kind = "exception";
			f.File = c.File;
			f.Line = c.Line;
			f.Message = "an exception escaped the case (not a std::exception)";
			Record(std::move(f));
		}
#else
		c.Fn();
#endif
		const bool pending = !s.Sub.PendingAt.empty() && s.Sub.PendingAt[0];
		if (!pending && !s.Sub.Incomplete)
			break;
	}
	std::lock_guard<std::mutex> lock(s.Lock);
	CaseResult r;
	r.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	r.Assertions = s.Assertions;
	r.Failures = s.Failures.size();
	r.Status = r.Failures ? "fail" : "pass";
	Emit("{\"event\":\"case-end\",\"name\":" + JsonString(c.Name) + ",\"status\":" + JsonString(r.Status) +
		",\"seconds\":" + std::to_string(r.Seconds) + ",\"assertions\":" + std::to_string(r.Assertions) + "}");
	s.Current = nullptr;
	return r;
}

} // namespace detail

// The cases, in registration order (the order of their files' inclusion).
inline const std::vector<Case>& Cases() { return detail::Registry(); }

inline constexpr const char kUsage[] =
	"usage: <test binary> [options]\n"
	"  --case <glob>       run only the cases whose name matches (repeatable)\n"
	"  --exclude <glob>    run none of the cases whose name matches (repeatable)\n"
	"  --list              print the cases' names\n"
	"  --no-skip           also run the cases marked skip\n"
	"  --list-json <file>  write the cases as JSON lines (hamr's discovery)\n"
	"  --run <file>        run the cases named in the file, one per line (hamr's batches)\n"
	"  --events <file>     write what each case did as JSON lines\n"
	"  --quiet             no console output but the summary\n";

// Runs the binary's cases as the arguments say; the process's exit code:
// 0 when every case run passed, 1 when one failed, 2 for a usage error.
// Arguments it does not know are an error, so a custom main takes its own
// out first.
inline int Run(int argc, char** argv) {
	using namespace detail;
	std::vector<std::string> include, exclude;
	std::string listJson, runFile, eventsFile;
	bool list = false, noSkip = false, quiet = false;
	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		auto value = [&](std::string& out) -> bool {
			if (i + 1 >= argc)
				return false;
			out = argv[++i];
			return true;
		};
		std::string v;
		if (arg == "--case" && value(v))
			include.push_back(v);
		else if (arg.rfind("--case=", 0) == 0)
			include.push_back(arg.substr(7));
		else if (arg == "--exclude" && value(v))
			exclude.push_back(v);
		else if (arg.rfind("--exclude=", 0) == 0)
			exclude.push_back(arg.substr(10));
		else if (arg == "--list")
			list = true;
		else if (arg == "--no-skip")
			noSkip = true;
		else if (arg == "--quiet")
			quiet = true;
		else if (arg == "--list-json" && value(v))
			listJson = v;
		else if (arg == "--run" && value(v))
			runFile = v;
		else if (arg == "--events" && value(v))
			eventsFile = v;
		else if (arg == "--help" || arg == "-h") {
			std::fputs(kUsage, stdout);
			return 0;
		} else {
			std::fprintf(stderr, "unknown argument '%s'\n%s", arg.c_str(), kUsage);
			return 2;
		}
	}

	// Case names must be unique: hamr runs and reports cases by name.
	{
		std::map<std::string, const Case*> seen;
		for (const Case& c : Cases()) {
			auto [it, fresh] = seen.emplace(c.Name, &c);
			if (!fresh) {
				std::fprintf(stderr, "two cases are named '%s' (%s:%d and %s:%d); a case's name must be unique\n", c.Name.c_str(),
					it->second->File, it->second->Line, c.File, c.Line);
				return 2;
			}
		}
	}

	std::vector<const Case*> chosen;
	if (!runFile.empty()) {
		// hamr's batch: exactly these cases, in this order.
		std::ifstream in(runFile);
		std::map<std::string, const Case*> byName;
		for (const Case& c : Cases())
			byName.emplace(c.Name, &c);
		std::string line;
		while (std::getline(in, line)) {
			if (line.empty())
				continue;
			auto it = byName.find(line);
			if (it == byName.end()) {
				std::fprintf(stderr, "no case named '%s'\n", line.c_str());
				return 2;
			}
			chosen.push_back(it->second);
		}
	} else {
		for (const Case& c : Cases()) {
			bool in = include.empty();
			for (const std::string& g : include)
				in = in || Match(g.c_str(), c.Name.c_str());
			for (const std::string& g : exclude)
				in = in && !Match(g.c_str(), c.Name.c_str());
			if (in)
				chosen.push_back(&c);
		}
	}

	if (!listJson.empty()) {
		std::ofstream out(listJson);
		for (const Case& c : Cases())
			out << "{\"name\":" << JsonString(c.Name) << ",\"file\":" << JsonString(c.File) << ",\"line\":" << c.Line
				<< ",\"timeout\":" << c.Timeout << ",\"skip\":" << (c.Skip ? "true" : "false") << ",\"tags\":" << JsonString(c.Tags)
				<< "}\n";
		return out ? 0 : 2;
	}
	if (list) {
		for (const Case* c : chosen)
			std::printf("%s\n", c->Name.c_str());
		return 0;
	}

	State& s = St();
	s.Console = !quiet;
	if (!eventsFile.empty()) {
		s.Events = std::fopen(eventsFile.c_str(), "a");
		if (!s.Events) {
			std::fprintf(stderr, "cannot write %s\n", eventsFile.c_str());
			return 2;
		}
	}
	int passed = 0, failed = 0, skipped = 0, assertions = 0;
	const bool batch = !runFile.empty();
	for (const Case* c : chosen) {
		if (c->Skip && !noSkip && !batch) {
			skipped++;
			continue;
		}
		CaseResult r = RunCase(*c);
		assertions += r.Assertions;
		if (r.Status == "pass")
			passed++;
		else
			failed++;
	}
	std::fprintf(stderr, "hamr test: %d case(s): %d passed, %d failed, %d skipped; %d assertion(s)\n", passed + failed + skipped,
		passed, failed, skipped, assertions);
	if (s.Events)
		std::fclose(s.Events);
	s.Events = nullptr;
	return failed ? 1 : 0;
}

} // namespace hamr::test

// ---- the macros --------------------------------------------------------------------

#define HAMR_TEST_CAT_(a, b) a##b
#define HAMR_TEST_CAT(a, b) HAMR_TEST_CAT_(a, b)
// Unique names come from __COUNTER__: by line alone, two cases on the same
// line of two headers one translation unit includes would collide. Clang's
// pedantic mode calls __COUNTER__ an extension; every compiler hamr targets
// has it, so that note is turned off for the rest of the translation unit.
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wc2y-extensions"
#endif
#define HAMR_TEST_UNIQUE(name) HAMR_TEST_CAT(name, __COUNTER__)

#define HAMR_TEST_CASE_(fn, spec)                                                                                     \
	static void fn();                                                                                                 \
	static const ::hamr::test::detail::Registrar HAMR_TEST_CAT(fn, _registrar)(spec, __FILE__, __LINE__, &fn);        \
	static void fn()

// A test case: TEST_CASE("name") or TEST_CASE("name" * hamr::test::timeout(5) * hamr::test::skip()).
#define TEST_CASE(spec) HAMR_TEST_CASE_(HAMR_TEST_UNIQUE(hamr_test_case_), spec)

// A part of a case run on its own: the case runs once per leaf subcase.
#define SUBCASE(name) if (const ::hamr::test::detail::Subcase HAMR_TEST_UNIQUE(hamr_test_subcase_){name})

#define HAMR_TEST_MESSAGE(msg) ((::hamr::test::detail::MessageBuilder() << msg).Str())

// `Decomposer() <= a == b` is how an expression is taken apart, so the
// compilers' "suggest parentheses" warning does not apply inside it.
#if defined(__GNUC__)
#define HAMR_TEST_QUIET_BEGIN_ _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wparentheses\"")
#define HAMR_TEST_QUIET_END_ _Pragma("GCC diagnostic pop")
#else
#define HAMR_TEST_QUIET_BEGIN_
#define HAMR_TEST_QUIET_END_
#endif
#define HAMR_TEST_DECOMPOSE_(...) ::hamr::test::detail::Decomposer() <= __VA_ARGS__

#if HAMR_TEST_EXCEPTIONS
#define HAMR_TEST_EVAL_(macro, require, negate, message, warn, ...)                                                      \
	do {                                                                                                               \
		try {                                                                                                          \
			HAMR_TEST_QUIET_BEGIN_                                                                                     \
			const ::hamr::test::detail::Result hamr_test_result_ = HAMR_TEST_DECOMPOSE_(__VA_ARGS__);                  \
			HAMR_TEST_QUIET_END_                                                                                       \
			::hamr::test::detail::Assert(macro, require, negate, hamr_test_result_, #__VA_ARGS__, __FILE__, __LINE__, \
				message, warn);                                                                                        \
		} catch (const ::hamr::test::detail::RequireAbort&) {                                                          \
			throw;                                                                                                     \
		} catch (const std::exception& hamr_test_e_) {                                                                 \
			::hamr::test::detail::Assert(macro, require, negate, {negate, std::string("threw: ") + hamr_test_e_.what()}, \
				#__VA_ARGS__, __FILE__, __LINE__, message, warn);                                                      \
		} catch (...) {                                                                                                \
			::hamr::test::detail::Assert(macro, require, negate, {negate, "threw an exception"}, #__VA_ARGS__, __FILE__, \
				__LINE__, message, warn);                                                                              \
		}                                                                                                              \
	} while (false)
#else
#define HAMR_TEST_EVAL_(macro, require, negate, message, warn, ...)                                                      \
	do {                                                                                                               \
		HAMR_TEST_QUIET_BEGIN_                                                                                         \
		const ::hamr::test::detail::Result hamr_test_result_ = HAMR_TEST_DECOMPOSE_(__VA_ARGS__);                      \
		HAMR_TEST_QUIET_END_                                                                                           \
		::hamr::test::detail::Assert(macro, require, negate, hamr_test_result_, #__VA_ARGS__, __FILE__, __LINE__,     \
			message, warn);                                                                                            \
	} while (false)
#endif

#define CHECK(...) HAMR_TEST_EVAL_("CHECK", false, false, std::string(), false, __VA_ARGS__)
#define REQUIRE(...) HAMR_TEST_EVAL_("REQUIRE", true, false, std::string(), false, __VA_ARGS__)
#define WARN(...) HAMR_TEST_EVAL_("WARN", false, false, std::string(), true, __VA_ARGS__)
#define CHECK_FALSE(...) HAMR_TEST_EVAL_("CHECK_FALSE", false, true, std::string(), false, __VA_ARGS__)
#define REQUIRE_FALSE(...) HAMR_TEST_EVAL_("REQUIRE_FALSE", true, true, std::string(), false, __VA_ARGS__)
#define CHECK_MESSAGE(cond, msg) HAMR_TEST_EVAL_("CHECK", false, false, HAMR_TEST_MESSAGE(msg), false, cond)
#define REQUIRE_MESSAGE(cond, msg) HAMR_TEST_EVAL_("REQUIRE", true, false, HAMR_TEST_MESSAGE(msg), false, cond)
#define WARN_MESSAGE(cond, msg) HAMR_TEST_EVAL_("WARN", false, false, HAMR_TEST_MESSAGE(msg), true, cond)
#define CHECK_FALSE_MESSAGE(cond, msg) HAMR_TEST_EVAL_("CHECK_FALSE", false, true, HAMR_TEST_MESSAGE(msg), false, cond)
#define REQUIRE_FALSE_MESSAGE(cond, msg) HAMR_TEST_EVAL_("REQUIRE_FALSE", true, true, HAMR_TEST_MESSAGE(msg), false, cond)

#define HAMR_TEST_BINARY_(name, require, op, a, b) HAMR_TEST_EVAL_(name, require, false, std::string(), false, (a) op (b))
#define CHECK_EQ(a, b) HAMR_TEST_BINARY_("CHECK_EQ", false, ==, a, b)
#define CHECK_NE(a, b) HAMR_TEST_BINARY_("CHECK_NE", false, !=, a, b)
#define CHECK_LT(a, b) HAMR_TEST_BINARY_("CHECK_LT", false, <, a, b)
#define CHECK_LE(a, b) HAMR_TEST_BINARY_("CHECK_LE", false, <=, a, b)
#define CHECK_GT(a, b) HAMR_TEST_BINARY_("CHECK_GT", false, >, a, b)
#define CHECK_GE(a, b) HAMR_TEST_BINARY_("CHECK_GE", false, >=, a, b)
#define REQUIRE_EQ(a, b) HAMR_TEST_BINARY_("REQUIRE_EQ", true, ==, a, b)
#define REQUIRE_NE(a, b) HAMR_TEST_BINARY_("REQUIRE_NE", true, !=, a, b)
#define REQUIRE_LT(a, b) HAMR_TEST_BINARY_("REQUIRE_LT", true, <, a, b)
#define REQUIRE_LE(a, b) HAMR_TEST_BINARY_("REQUIRE_LE", true, <=, a, b)
#define REQUIRE_GT(a, b) HAMR_TEST_BINARY_("REQUIRE_GT", true, >, a, b)
#define REQUIRE_GE(a, b) HAMR_TEST_BINARY_("REQUIRE_GE", true, >=, a, b)

#if HAMR_TEST_EXCEPTIONS
#define HAMR_TEST_THROWS_(macro, require, expected, note, catches, ...)                                                  \
	do {                                                                                                               \
		bool hamr_test_threw_ = false;                                                                                 \
		try {                                                                                                          \
			static_cast<void>(__VA_ARGS__);                                                                            \
		} catch (const ::hamr::test::detail::RequireAbort&) {                                                          \
			throw;                                                                                                     \
		} catches                                                                                                      \
		::hamr::test::detail::Throws(macro, require, hamr_test_threw_, expected, #__VA_ARGS__, __FILE__, __LINE__, note); \
	} while (false)
#define HAMR_TEST_ANY_ catch (...) { hamr_test_threw_ = true; }
#define CHECK_THROWS(...) HAMR_TEST_THROWS_("CHECK_THROWS", false, true, "it did not throw", HAMR_TEST_ANY_, __VA_ARGS__)
#define REQUIRE_THROWS(...) HAMR_TEST_THROWS_("REQUIRE_THROWS", true, true, "it did not throw", HAMR_TEST_ANY_, __VA_ARGS__)
#define CHECK_NOTHROW(...) HAMR_TEST_THROWS_("CHECK_NOTHROW", false, false, "it threw", HAMR_TEST_ANY_, __VA_ARGS__)
#define REQUIRE_NOTHROW(...) HAMR_TEST_THROWS_("REQUIRE_NOTHROW", true, false, "it threw", HAMR_TEST_ANY_, __VA_ARGS__)
// Throws an exception of `type` (another exception is a failure too).
#define CHECK_THROWS_AS(expr, ...)                                                                                     \
	HAMR_TEST_THROWS_("CHECK_THROWS_AS", false, true, "it did not throw a " #__VA_ARGS__,                             \
		catch (const __VA_ARGS__&) { hamr_test_threw_ = true; } catch (...) {}, expr)
#define REQUIRE_THROWS_AS(expr, ...)                                                                                   \
	HAMR_TEST_THROWS_("REQUIRE_THROWS_AS", true, true, "it did not throw a " #__VA_ARGS__,                            \
		catch (const __VA_ARGS__&) { hamr_test_threw_ = true; } catch (...) {}, expr)
#endif

// Context printed with a failure later in the same scope.
#define INFO(msg)                                                                                                      \
	const ::hamr::test::detail::ScopedInfo HAMR_TEST_UNIQUE(hamr_test_info_)([&]() -> std::string {                  \
		return HAMR_TEST_MESSAGE(msg);                                                                                 \
	})
#define CAPTURE(x) INFO(#x " := " << ::hamr::test::ToString(x))

// A message recorded with the case, not a failure.
#define MESSAGE(msg) ::hamr::test::detail::Message(__FILE__, __LINE__, HAMR_TEST_MESSAGE(msg))
// A failure that ends the case, and one that does not.
#define FAIL(msg)                                                                                                      \
	do {                                                                                                               \
		::hamr::test::detail::Fail(__FILE__, __LINE__, HAMR_TEST_MESSAGE(msg));                                         \
		::hamr::test::detail::AbortCase();                                                                             \
	} while (false)
#define FAIL_CHECK(msg) ::hamr::test::detail::Fail(__FILE__, __LINE__, HAMR_TEST_MESSAGE(msg))

#ifdef HAMR_TEST_MAIN
int main(int argc, char** argv) { return ::hamr::test::Run(argc, argv); }
#endif
