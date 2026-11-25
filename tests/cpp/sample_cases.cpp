// The sample cases hamr's own suite runs through the native adapter: cases
// that pass, fail in each way, crash, hang, turn out flaky, check from many
// threads, use subcases or are skipped. The misbehaving ones act only when
// HAMR_SAMPLE names them, so the binary run on its own passes.

#define HAMR_TEST_MAIN
#include "hamr/test.h"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

bool Enabled(const char* what) {
	const char* list = std::getenv("HAMR_SAMPLE");
	return list && std::strstr(list, what) != nullptr;
}

struct Point {
	int X, Y;
	bool operator==(const Point& o) const { return X == o.X && Y == o.Y; }
};
std::ostream& operator<<(std::ostream& out, const Point& p) { return out << "(" << p.X << ", " << p.Y << ")"; }

} // namespace

TEST_CASE("passes: the comparisons, Approx and the exception checks") {
	std::vector<int> v{1, 2, 3};
	REQUIRE(v.size() == 3);
	CHECK(v[0] == 1);
	CHECK(v[1] != 3);
	CHECK(v[2] > 2 - 1);
	CHECK_EQ(v.front(), 1);
	CHECK_LT(v.front(), v.back());
	CHECK_FALSE(v.empty());
	CHECK(0.1 + 0.2 == hamr::test::Approx(0.3));
	CHECK(1.0 != hamr::test::Approx(1.1));
	CHECK(100.0 == hamr::test::Approx(101.0).epsilon(0.02));
	CHECK(Point{1, 2} == Point{1, 2});
	CHECK(std::string("abc") == "abc");
	CHECK_THROWS_AS(throw std::runtime_error("x"), std::runtime_error);
	CHECK_THROWS(throw 3);
	CHECK_NOTHROW(v.at(0));
	CHECK_MESSAGE(v.size() == 3, "the vector has " << v.size() << " elements");
}

TEST_CASE("fails: a check, a message and context") {
	if (!Enabled("fail"))
		return;
	int answer = 41;
	INFO("the answer was computed by " << "the sample");
	CAPTURE(answer);
	CHECK(answer == 42);
	CHECK(Point{1, 2} == Point{2, 1});
	CHECK_MESSAGE(answer > 50, "answer is " << answer);
	CHECK(answer == 41); // still runs after a failed CHECK
}

TEST_CASE("fails: a require ends the case") {
	if (!Enabled("require"))
		return;
	REQUIRE(1 == 2);
	FAIL("not reached");
}

TEST_CASE("fails: an exception escapes") {
	if (Enabled("throw"))
		throw std::runtime_error("escaped");
}

TEST_CASE("crashes") {
	if (Enabled("crash"))
		std::raise(SIGSEGV);
}

TEST_CASE("hangs" * hamr::test::timeout(1)) {
	if (Enabled("hang"))
		std::this_thread::sleep_for(std::chrono::seconds(30));
}

TEST_CASE("flaky: the case that leaves the file") {
	const char* marker = std::getenv("HAMR_SAMPLE_MARKER");
	if (Enabled("flaky") && marker) {
		if (std::FILE* f = std::fopen(marker, "w"))
			std::fclose(f);
	}
}

TEST_CASE("flaky: fails beside another case, passes alone") {
	// The batch-mates leave a file behind; alone, it is not there.
	if (!Enabled("flaky"))
		return;
	const char* marker = std::getenv("HAMR_SAMPLE_MARKER");
	if (!marker)
		return;
	std::FILE* f = std::fopen(marker, "r");
	if (f) {
		std::fclose(f);
		std::remove(marker);
		CHECK(false);
	}
}

TEST_CASE("subcases: each leaf once") {
	MESSAGE("enter");
	SUBCASE("a") {
		MESSAGE("a");
		SUBCASE("a1") { MESSAGE("a1"); }
		SUBCASE("a2") { MESSAGE("a2"); }
	}
	SUBCASE("b") { MESSAGE("b"); }
}

TEST_CASE("threads: checks from a case's worker threads") {
	std::vector<std::thread> threads;
	for (int t = 0; t < 8; t++)
		threads.emplace_back([t] {
			INFO("thread " << t);
			for (int i = 0; i < 1000; i++)
				CHECK(i + t >= t);
		});
	for (std::thread& thread : threads)
		thread.join();
}

TEST_CASE("chars read as themselves") {
	if (Enabled("fail"))
		CHECK('a' == 'b');
}

TEST_CASE("skipped" * hamr::test::skip()) {
	CHECK(false);
}

TEST_CASE("slow: half a second") {
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	CHECK(true);
}
