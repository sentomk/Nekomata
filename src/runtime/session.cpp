#include <neko/backend/session_driver.hpp>

#include <stdexcept>
#include <utility>

namespace neko::backend {

void session_driver::watch(std::filesystem::path) {
  throw std::runtime_error("reload_session: object watches are not supported by this backend");
}

void session_driver::watch(std::filesystem::path, const std::filesystem::path&) {
  throw std::runtime_error("reload_session: object watches are not supported by this backend");
}

} // namespace neko::backend

namespace neko {

reload_session::reload_session(std::unique_ptr<backend::session_driver> driver)
    : impl_(std::move(driver)) {
  if (!impl_) {
    throw std::runtime_error("reload_session: null session driver");
  }
}

reload_session::~reload_session() = default;

reload_session::reload_session(reload_session&&) noexcept = default;
reload_session& reload_session::operator=(reload_session&&) noexcept = default;

void reload_session::watch(std::filesystem::path object_path) {
  impl_->watch(std::move(object_path));
}

void reload_session::watch(std::filesystem::path object_path,
                           const std::filesystem::path& source_path) {
  impl_->watch(std::move(object_path), source_path);
}

void reload_session::watch() {
  impl_->watch();
}

void reload_session::watch(std::string_view group_id) {
  impl_->watch(group_id);
}

void reload_session::watch(const char* group_id) {
  impl_->watch(std::string_view{group_id});
}

void reload_session::unwatch() {
  impl_->unwatch();
}

void reload_session::unwatch(std::string_view group_id) {
  impl_->unwatch(group_id);
}

void reload_session::unwatch(const char* group_id) {
  impl_->unwatch(std::string_view{group_id});
}

update_result reload_session::update() {
  return impl_->update();
}

session_snapshot reload_session::snapshot() const {
  return impl_->snapshot();
}

} // namespace neko
