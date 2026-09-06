// BinaryStore.cc — vtable anchor + destructor for the pure virtual interface
#include "BinaryStore.h"

namespace sila2 {

// Out-of-line virtual destructor provides a vtable anchor and ensures the
// GC thread is joined before the derived class's PeriodicGC member is
// destroyed. Derived destructors should also call stopAutoGC() to avoid a
// virtual dispatch during base-class destruction.
BinaryStore::~BinaryStore() {
    stopAutoGC();
}

}  // namespace sila2
