// Host check for patches/scintilla-load-fast.patch: SplitVector<char>::InsertValue (doubling memcpy fill)
// must give the same bytes as a plain fill for every length around the 16-byte seed and the doubling steps,
// at every gap position. Build: c++ -std=c++17 -DNDEBUG -I src/scintilla/scintilla/src -I src/scintilla/scintilla/include \
//   tools/splitvector_fill_check.cxx -o /tmp/svcheck && /tmp/svcheck
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <vector>
#include <memory>
#include <type_traits>
#include <utility>
#include "Debugging.h"
#include "SplitVector.h"
using namespace Scintilla::Internal;
int main() {
	int bad = 0, cases = 0;
	const size_t lens[] = {1, 2, 15, 16, 17, 31, 32, 33, 47, 48, 100, 4095, 4096, 4097, 100000};
	for (size_t len : lens) {
		for (int pre : {0, 5, 40}) {
			SplitVector<char> sv;
			std::vector<char> ref;
			for (int i = 0; i < pre; i++) { sv.InsertValue(i, 1, char(i + 1)); ref.push_back(char(i + 1)); }
			const int at = pre / 2;
			sv.InsertValue(at, static_cast<ptrdiff_t>(len), char(7));
			ref.insert(ref.begin() + at, len, char(7));
			cases++;
			if (static_cast<size_t>(sv.Length()) != ref.size()) { bad++; continue; }
			for (size_t i = 0; i < ref.size(); i++) if (sv.ValueAt(static_cast<ptrdiff_t>(i)) != ref[i]) { bad++; break; }
		}
	}
	std::printf("splitvector fill: %d cases, %d bad\n", cases, bad);
	return bad != 0;
}
