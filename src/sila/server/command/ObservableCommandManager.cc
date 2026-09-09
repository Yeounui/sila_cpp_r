#include "ObservableCommandManager.h"

#include "ObservableCommandExecution.h"

#include <sila/common/error/SilaErrorSubtypes.h>
#include <sila/common/util/AsciiCase.h>
#include <sila/common/util/uuid.h>

#include <utility>
#include <vector>

namespace sila2 {
/*  = default:
    기본 생성자(인자 없는 생성자)를 컴파일러가 자동 생성하게 허용.
    컴파일러에게 "사용자 정의 아님(trivial일 수 있음)"이라는 힌트를 추가로 줘서 최적화 여지가 더 넓음.
*/
ObservableCommandManager::ObservableCommandManager() = default;

ObservableCommandManager::~ObservableCommandManager() {
    stopAutoGC();
}

std::shared_ptr<ObservableCommandExecution> ObservableCommandManager::addCommand(
    std::chrono::seconds lifetime) {
    std::string uuid = util::generateUuid();
    // Construct the execution before taking mu_: the constructor does no
    // shared-state work, so there is nothing to protect yet, and keeping it
    // out of the critical section means the lock is only ever held for the
    // map mutation itself.
    /*  commands_는 unique_ptr가 아닌 shared_ptr로 소유 (architecture.md §4.2d).
        이유: getCommand()가 mu_ 해제 후 bare reference를 돌려주면,
        반환 직후 다른 스레드의 removeExpired() GC 스윕이 같은 항목을 erase할 때
        호출자가 쥔 reference가 dangling됨. shared_ptr을 값으로 반환하면
        호출자가 사본을 쥐고 있는 한 참조 카운트가 유지되어 그 객체는 살아있음.
        std::make_shared: new + shared_ptr 생성자를 따로 안 쓰고 컨트롤 블록과
        객체를 한 번의 할당으로 묶어 생성 (make_unique와 동일한 예외 안전성 이점).
    */
    auto execution = std::make_shared<ObservableCommandExecution>(uuid, lifetime);
    /*  std::lock_guard<std::mutex> lock{mu_}
        mu_를 잠그고, lock이 스코프를 벗어나면(함수 리턴 시) 자동으로 풀림(RAII).
        commands_.emplace() 반환 타입은 std::pair<iterator, bool>,
        여기서 iterator는 unordered_map (commands_)의 key-value 한 쌍 (entry).
        auto가 오른쪽 표현식 반환 타입을 통해 컴파일 타임에 치환.

        iterator의 각 인자 이름은 first, second. 따라서 entry의 value = execution.
    */
    std::lock_guard<std::mutex> lock{mu_};
    auto [it, inserted] = commands_.emplace(std::move(uuid), std::move(execution));
    return it->second;
}

std::shared_ptr<ObservableCommandExecution> ObservableCommandManager::getCommand(
    const std::string& uuid) {
    // Part A p90 / Part B p88: UUID comparison MUST ignore case. The server
    // issues lower-case UUIDs (uuid.cc) so the stored key is lower-case; lower
    // the client-returned lookup key to match, without changing the map above
    // (keeps the unordered_map its comment documents).
    const std::string key = util::asciiLower(uuid);
    std::lock_guard<std::mutex> lock{mu_};
    auto it = commands_.find(key);
    if (it == commands_.end()) {
        throw sila2::error::FrameworkError{
            sila2::error::FrameworkError::FrameworkErrorType::InvalidCommandExecutionUuid};
    }
    return it->second;
}

std::size_t ObservableCommandManager::removeExpired() {
    // Collect the removed UUIDs and a copy of removalObservers_ under mu_,
    // then invoke the observers after unlocking (architecture.md §2.2c):
    // observers include CloudEnvelopeRouter::removeExecutionFQI, which
    // locks CloudEnvelopeRouter::mu_. Calling it while still holding this
    // manager's mu_ risks deadlock against any path that acquires the two
    // locks in the opposite order. Copying the whole list (rather than just
    // one callback) keeps that same after-unlock invocation shape now that
    // multiple independent observers (Cloud, gRPC owner registry) share the slot.
    std::vector<std::string> removedUuids;
    std::vector<RemovalCallback> observers;
    {
        std::lock_guard<std::mutex> lock{mu_};
        observers = removalObservers_;
        for (auto it = commands_.begin(); it != commands_.end();) {
            if (it->second->isExpired()) {
                removedUuids.push_back(it->first);
                it = commands_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& observer : observers) {
        for (const auto& uuid : removedUuids) { observer(uuid); }
    }
    return removedUuids.size();
}

void ObservableCommandManager::interruptAll() {
    std::lock_guard<std::mutex> lock{mu_};
    for (auto& [uuid, execution] : commands_) { execution->requestInterruption(); }
}

void ObservableCommandManager::addRemovalObserver(RemovalCallback cb) {
    std::lock_guard<std::mutex> lock{mu_};
    removalObservers_.push_back(std::move(cb));
}

void ObservableCommandManager::clearRemovalObservers() {
    std::lock_guard<std::mutex> lock{mu_};
    removalObservers_.clear();
}

std::size_t ObservableCommandManager::size() const {
    std::lock_guard<std::mutex> lock{mu_};
    return commands_.size();
}

}  // namespace sila2
