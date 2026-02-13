#include "load_balance.h"

namespace rpc {
class RanceLoad : public LoadBanlance {
 public:
  RanceLoad() : current_index_(0){};
  std::string Select(std::vector<std::string>& servers, std::string& client) {
    if (servers.empty()) {
      return "";
    }
    size_t res = current_index_.fetch_add(1) % servers.size();
    return servers[res];
  }

 private:
  std::atomic<size_t> current_index_;
};
}  // namespace rpc