// Force-included ahead of ../letterlock.cpp (-include) for DJGPP:
//  - its libstdc++ is built without C99 math in namespace std, so bring the
//    ones the game calls in from the global namespace;
//  - its <math.h> defines PI as a macro, which clashes with the game's own PI.
#pragma once
#include <cmath>
#undef PI
namespace std {
using ::lround;
using ::lroundf;
}
