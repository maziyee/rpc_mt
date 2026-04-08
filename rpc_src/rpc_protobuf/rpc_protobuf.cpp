#include "rpc_protobuf.h"

namespace rpc {

const uint32_t RpcHeader::kMagic = 0x12345678;

bool rpc::RpcRequest::Serializer(std::string& out) {
  try {
    size_t body_size = this->method_name_.size() + this->service_name_.size() +
                       this->payload_.size() + 3 * sizeof(uint32_t);
    RpcHeader header;
    out.resize(body_size + sizeof(RpcHeader));
    header.body_size_ = body_size;
    header.magic_ = RpcHeader::kMagic;
    header.sequence_id_ = this->GetSequenceId();
    std::memcpy(&out[0], &header, sizeof(RpcHeader));
    size_t pos = sizeof(RpcHeader);

    uint32_t service_name_size = this->service_name_.size();
    std::memcpy(&out[pos], &service_name_size, sizeof(uint32_t));
    pos += sizeof(uint32_t);

    std::memcpy(&out[pos], this->service_name_.c_str(), service_name_size);
    pos += service_name_size;

    uint32_t method_name_size = this->method_name_.size();
    std::memcpy(&out[pos], &method_name_size, sizeof(uint32_t));
    pos += sizeof(uint32_t);

    std::memcpy(&out[pos], this->method_name_.c_str(), method_name_size);
    pos += method_name_size;

    uint32_t payload_size = this->payload_.size();
    std::memcpy(&out[pos], &payload_size, sizeof(uint32_t));
    pos += sizeof(uint32_t);

    std::memcpy(&out[pos], this->payload_.c_str(), payload_size);
    pos += payload_size;

    return true;

  } catch (std::exception& e) {
    LOG_ERROR("RpcRequest::Serializer error: {}", e.what());
    return false;
  }
}

bool RpcRequest::Deserializer(const std::string& in) {
  try {
    if (in.size() < sizeof(RpcHeader)) {
      LOG_ERROR("RpcRequest::Deserializer error: input size is too small");
      return false;
    }
    RpcHeader header;
    std::memcpy(&header, in.data(), sizeof(RpcHeader));
    if (header.magic_ != RpcHeader::kMagic) {
      LOG_ERROR("RpcRequest::Deserializer error: magic number is not valid");
      return false;
    }
    size_t pos = sizeof(RpcHeader);
    size_t body_size = header.body_size_;
    if (in.size() < body_size + sizeof(RpcHeader)) {
      LOG_ERROR("RpcRequest::Deserializer error: input size is too small");
      return false;
    }

    uint32_t service_name_size;
    if (pos + sizeof(uint32_t) > in.size()) {
      LOG_ERROR("RpcRequest::Deserializer error: input size is too small");
      return false;
    }
    std::memcpy(&service_name_size, &in[pos], sizeof(uint32_t));
    pos += sizeof(uint32_t);

    if (pos + service_name_size > in.size()) {
      LOG_ERROR("RpcRequest::Deserializer error: input size is too small");
      return false;
    }
    this->service_name_ = in.substr(pos, service_name_size);
    pos += service_name_size;

    uint32_t method_name_size;
    if (pos + sizeof(uint32_t) > in.size()) {
      LOG_ERROR("RpcRequest::Deserializer error: input size is too small");
      return false;
    }
    std::memcpy(&method_name_size, &in[pos], sizeof(uint32_t));
    pos += sizeof(uint32_t);

    if (pos + method_name_size > in.size()) {
      LOG_ERROR("RpcRequest::Deserializer error: input size is too small");
      return false;
    }
    this->method_name_ = in.substr(pos, method_name_size);
    pos += method_name_size;

    uint32_t payload_size;
    if (pos + sizeof(uint32_t) > in.size()) {
      LOG_ERROR("RpcRequest::Deserializer error: input size is too small");
      return false;
    }
    std::memcpy(&payload_size, &in[pos], sizeof(uint32_t));
    pos += sizeof(uint32_t);

    if (pos + payload_size > in.size()) {
      LOG_ERROR("RpcRequest::Deserializer error: input size is too small");
      return false;
    }
    this->payload_ = in.substr(pos, payload_size);
    pos += payload_size;

    this->SetSequenceId(header.sequence_id_);
    return true;

  } catch (std::exception& e) {
    LOG_ERROR("RpcRequest::Deserializer error: {}", e.what());
    return false;
  }
}

bool RpcResponse::Serializer(std::string& out) {
  try {
    size_t body_size = this->error_message_.size() + this->result_data_.size() +
                       2 * sizeof(uint32_t) + sizeof(this->error_code_) +
                       sizeof(this->GetSequenceId());
    RpcHeader header;
    out.resize(body_size + sizeof(RpcHeader));
    header.body_size_ = body_size;
    header.magic_ = RpcHeader::kMagic;
    header.sequence_id_ = this->GetSequenceId();
    std::memcpy(&out[0], &header, sizeof(RpcHeader));
    size_t pos = sizeof(RpcHeader);

    uint32_t result_data_size = this->result_data_.size();
    std::memcpy(&out[pos], &result_data_size, sizeof(uint32_t));
    pos += sizeof(uint32_t);

    std::memcpy(&out[pos], this->result_data_.c_str(), result_data_size);
    pos += result_data_size;

    uint32_t error_message_size = this->error_message_.size();
    std::memcpy(&out[pos], &error_message_size, sizeof(uint32_t));
    pos += sizeof(uint32_t);

    std::memcpy(&out[pos], this->error_message_.c_str(), error_message_size);
    pos += error_message_size;

    std::memcpy(&out[pos], &this->error_code_, sizeof(this->error_code_));
    pos += sizeof(this->error_code_);

    std::memcpy(&out[pos], &header.sequence_id_, sizeof(this->GetSequenceId()));
    pos += sizeof(this->GetSequenceId());

    return true;

  } catch (std::exception& e) {
    LOG_ERROR("RpcResponse::Serializer error: {}", e.what());
    return false;
  }
}

bool RpcResponse::Deserializer(const std::string& in) {
  try {
    if (in.size() < sizeof(RpcHeader)) {
      LOG_ERROR("RpcResponse::Deserializer error: input size is too small");
      return false;
    }
    RpcHeader header;
    std::memcpy(&header, in.data(), sizeof(RpcHeader));
    if (header.magic_ != RpcHeader::kMagic) {
      LOG_ERROR("RpcResponse::Deserializer error: magic number is not valid");
      return false;
    }
    size_t pos = sizeof(RpcHeader);

    if (in.size() != header.body_size_ + sizeof(RpcHeader)) {
      LOG_ERROR(
          "RpcResponse::Deserializer error: input size is not equal to body "
          "size");
      return false;
    }

    uint32_t result_data_size;
    if (pos + sizeof(uint32_t) > in.size()) {
      LOG_ERROR("RpcResponse::Deserializer error: input size is too small");
      return false;
    }
    std::memcpy(&result_data_size, &in[pos], sizeof(uint32_t));
    pos += sizeof(uint32_t);

    if (pos + result_data_size > in.size()) {
      LOG_ERROR("RpcResponse::Deserializer error: input size is too small");
      return false;
    }
    this->result_data_ = in.substr(pos, result_data_size);
    pos += result_data_size;

    uint32_t error_message_size;
    if (pos + sizeof(uint32_t) > in.size()) {
      LOG_ERROR("RpcResponse::Deserializer error: input size is too small");
      return false;
    }
    std::memcpy(&error_message_size, &in[pos], sizeof(uint32_t));
    pos += sizeof(uint32_t);

    if (pos + error_message_size > in.size()) {
      LOG_ERROR("RpcResponse::Deserializer error: input size is too small");
      return false;
    }
    this->error_message_ = in.substr(pos, error_message_size);
    pos += error_message_size;

    if (pos + sizeof(this->error_code_) > in.size()) {
      LOG_ERROR("RpcResponse::Deserializer error: input size is too small");
      return false;
    }
    std::memcpy(&this->error_code_, &in[pos], sizeof(this->error_code_));
    pos += sizeof(this->error_code_);

    if (pos + sizeof(this->GetSequenceId()) > in.size()) {
      LOG_ERROR("RpcResponse::Deserializer error: input size is too small");
      return false;
    }
    uint32_t temp_sequence_id;
    std::memcpy(&temp_sequence_id, &in[pos], sizeof(temp_sequence_id));
    this->SetSequenceId(temp_sequence_id);
    pos += sizeof(temp_sequence_id);
  } catch (std::exception& e) {
    LOG_ERROR("RpcResponse::Deserializer error: {}", e.what());
    return false;
  }
  return true;
}

}  // namespace rpc