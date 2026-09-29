#pragma once
#include "melange/wormsign.h"
// The OnDivergence observers. The replay player and the desync detector report through Raise.
namespace melange::wormsign::divergence {
void Raise(const Divergence& d);               // main thread; every observer, in registration order
}
