#include <functional>
#include <string>

#include "load_balance.h"

namespace rpc {
class HashLoad : public LoadBanlance {
 public:
  std::string Select(std::vector<std::string>& servers, std::string& client) {
    if (servers.empty()) {
      return "";
    }
    size_t value = std::hash<std::string>{}(client);
    size_t index = value % servers.size();
    return servers[index];
  }
};
}  // namespace rpc