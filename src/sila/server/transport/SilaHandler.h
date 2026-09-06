// SilaHandler.h — typed handler signature Feature implementations define
// (architecture.md §3.8)
//
// New component. GrpcTransport and CloudEnvelopeRouter each adapt their
// native call model to invoke this signature.
#pragma once

#include <functional>

namespace sila2 {

class CallContext;
template <typename T>
class ResponseSink;

template <typename Req, typename Resp>
using SilaHandler = std::function<void(const Req&, CallContext&, ResponseSink<Resp>&)>;

}  // namespace sila2
