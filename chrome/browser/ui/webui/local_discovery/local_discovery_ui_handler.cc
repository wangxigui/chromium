// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/local_discovery/local_discovery_ui_handler.h"

#include "base/bind.h"
#include "base/strings/stringprintf.h"
#include "base/values.h"
#include "chrome/browser/local_discovery/privet_device_lister_impl.h"
#include "chrome/browser/local_discovery/privet_http_impl.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/signin/profile_oauth2_token_service.h"
#include "chrome/browser/signin/profile_oauth2_token_service_factory.h"
#include "chrome/browser/signin/signin_manager.h"
#include "chrome/browser/signin/signin_manager_base.h"
#include "chrome/browser/signin/signin_manager_factory.h"
#include "content/public/browser/web_ui.h"
#include "net/base/host_port_pair.h"
#include "net/base/net_util.h"
#include "net/http/http_status_code.h"

namespace local_discovery {

namespace {
// TODO(noamsml): This is a temporary shim until automated_url is in the
// response.
const char kPrivetAutomatedClaimURLFormat[] = "%s/confirm?token=%s";
const int kAccountIndexUseOAuth2 = -1;

LocalDiscoveryUIHandler::Factory* g_factory = NULL;
int g_num_visible = 0;
}  // namespace

LocalDiscoveryUIHandler::LocalDiscoveryUIHandler() : is_visible_(false) {
}

LocalDiscoveryUIHandler::LocalDiscoveryUIHandler(
    scoped_ptr<PrivetDeviceLister> privet_lister) {
  privet_lister.swap(privet_lister_);
}

LocalDiscoveryUIHandler::~LocalDiscoveryUIHandler() {
  SetIsVisible(false);
  if (service_discovery_client_.get()) {
    service_discovery_client_ = NULL;
    ServiceDiscoveryHostClientFactory::ReleaseClient();
  }
}

// static
LocalDiscoveryUIHandler* LocalDiscoveryUIHandler::Create() {
  if (g_factory) return g_factory->CreateLocalDiscoveryUIHandler();
  return new LocalDiscoveryUIHandler();
}

// static
void LocalDiscoveryUIHandler::SetFactory(Factory* factory) {
  g_factory = factory;
}

// static
bool LocalDiscoveryUIHandler::GetHasVisible() {
  return g_num_visible != 0;
}

void LocalDiscoveryUIHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback("start", base::Bind(
      &LocalDiscoveryUIHandler::HandleStart,
      base::Unretained(this)));
  web_ui()->RegisterMessageCallback("isVisible", base::Bind(
      &LocalDiscoveryUIHandler::HandleIsVisible,
      base::Unretained(this)));
  web_ui()->RegisterMessageCallback("registerDevice", base::Bind(
      &LocalDiscoveryUIHandler::HandleRegisterDevice,
      base::Unretained(this)));
  web_ui()->RegisterMessageCallback("info", base::Bind(
      &LocalDiscoveryUIHandler::HandleInfoRequested,
      base::Unretained(this)));
  web_ui()->RegisterMessageCallback("chooseUser", base::Bind(
      &LocalDiscoveryUIHandler::HandleChooseUser,
      base::Unretained(this)));
}

void LocalDiscoveryUIHandler::HandleStart(const base::ListValue* args) {
  // If privet_lister_ is already set, it is a mock used for tests or the result
  // of a reload.
  if (!privet_lister_) {
    service_discovery_client_ = ServiceDiscoveryHostClientFactory::GetClient();
    privet_lister_.reset(new PrivetDeviceListerImpl(
        service_discovery_client_.get(), this));
    privet_http_factory_.reset(new PrivetHTTPAsynchronousFactoryImpl(
        service_discovery_client_.get(),
        Profile::FromWebUI(web_ui())->GetRequestContext()));
  }

  privet_lister_->Start();
  privet_lister_->DiscoverNewDevices(false);
}

void LocalDiscoveryUIHandler::HandleRegisterDevice(
    const base::ListValue* args) {
  std::string device_name;

  bool rv = args->GetString(0, &device_name);
  DCHECK(rv);

  current_register_device_ = device_name;

  cloud_print_account_manager_.reset(new CloudPrintAccountManager(
      Profile::FromWebUI(web_ui())->GetRequestContext(),
      GetCloudPrintBaseUrl(device_name),
      0 /* Get XSRF token for primary user */,
      base::Bind(&LocalDiscoveryUIHandler::OnCloudPrintAccountsResolved,
                 base::Unretained(this))));

  cloud_print_account_manager_->Start();
}

void LocalDiscoveryUIHandler::HandleInfoRequested(const base::ListValue* args) {
  std::string device_name;
  args->GetString(0, &device_name);

  privet_resolution_ = privet_http_factory_->CreatePrivetHTTP(
      device_name,
      device_descriptions_[device_name].address,
      base::Bind(&LocalDiscoveryUIHandler::StartInfoHTTP,
                 base::Unretained(this)));
  privet_resolution_->Start();
}

void LocalDiscoveryUIHandler::HandleIsVisible(const base::ListValue* args) {
  bool is_visible = false;
  bool rv = args->GetBoolean(0, &is_visible);
  DCHECK(rv);
  SetIsVisible(is_visible);
}

void LocalDiscoveryUIHandler::HandleChooseUser(const base::ListValue* args) {
  std::string user;

  bool rv = args->GetInteger(0, &current_register_user_index_);
  DCHECK(rv);
  rv = args->GetString(1, &user);
  DCHECK(rv);

  privet_resolution_ = privet_http_factory_->CreatePrivetHTTP(
      current_register_device_,
      device_descriptions_[current_register_device_].address,
      base::Bind(&LocalDiscoveryUIHandler::StartRegisterHTTP,
                 base::Unretained(this), user));
  privet_resolution_->Start();
}

void LocalDiscoveryUIHandler::StartRegisterHTTP(
    const std::string& user,
    scoped_ptr<PrivetHTTPClient> http_client) {
  current_http_client_.swap(http_client);

  if (!current_http_client_) {
    LogRegisterErrorToWeb("Resolution failed");
    return;
  }

  current_register_operation_ =
      current_http_client_->CreateRegisterOperation(user, this);
  current_register_operation_->Start();
}

void LocalDiscoveryUIHandler::StartInfoHTTP(
    scoped_ptr<PrivetHTTPClient> http_client) {
  current_http_client_.swap(http_client);
  if (!current_http_client_) {
    LogRegisterErrorToWeb("Resolution failed");
    return;
  }

  current_info_operation_ = current_http_client_->CreateInfoOperation(this);
  current_info_operation_->Start();
}

void LocalDiscoveryUIHandler::OnPrivetRegisterClaimToken(
    PrivetRegisterOperation* operation,
    const std::string& token,
    const GURL& url) {
  if (device_descriptions_.count(current_http_client_->GetName()) == 0) {
    LogRegisterErrorToWeb("Device no longer exists");
    return;
  }

  std::string base_url = GetCloudPrintBaseUrl(current_http_client_->GetName());

  GURL automated_claim_url(base::StringPrintf(
      kPrivetAutomatedClaimURLFormat,
      base_url.c_str(),
      token.c_str()));

  Profile* profile = Profile::FromWebUI(web_ui());

  if (current_register_user_index_ == kAccountIndexUseOAuth2) {
    OAuth2TokenService* token_service =
        ProfileOAuth2TokenServiceFactory::GetForProfile(profile);

    if (!token_service) {
      LogRegisterErrorToWeb("Could not get token service");
      return;
    }

    confirm_api_call_flow_.reset(new PrivetConfirmApiCallFlow(
        profile->GetRequestContext(),
        token_service,
        automated_claim_url,
        base::Bind(&LocalDiscoveryUIHandler::OnConfirmDone,
                   base::Unretained(this))));
    confirm_api_call_flow_->Start();
  } else {
    if (current_register_user_index_ == 0) {
      StartCookieConfirmFlow(current_register_user_index_,
                             xsrf_token_for_primary_user_,
                             automated_claim_url);
    } else {
      cloud_print_account_manager_.reset(new CloudPrintAccountManager(
          Profile::FromWebUI(web_ui())->GetRequestContext(),
          base_url,
          current_register_user_index_,
          base::Bind(&LocalDiscoveryUIHandler::OnXSRFTokenForSecondaryAccount,
                     base::Unretained(this), automated_claim_url)));

      cloud_print_account_manager_->Start();
    }
  }
}

void LocalDiscoveryUIHandler::OnPrivetRegisterError(
    PrivetRegisterOperation* operation,
    const std::string& action,
    PrivetRegisterOperation::FailureReason reason,
    int printer_http_code,
    const DictionaryValue* json) {
  // TODO(noamsml): Add detailed error message.
  LogRegisterErrorToWeb("Registration error");
}

void LocalDiscoveryUIHandler::OnPrivetRegisterDone(
    PrivetRegisterOperation* operation,
    const std::string& device_id) {
  current_register_operation_.reset();
  current_http_client_.reset();

  LogRegisterDoneToWeb(device_id);
}

void LocalDiscoveryUIHandler::OnConfirmDone(
    PrivetConfirmApiCallFlow::Status status) {
  if (status == PrivetConfirmApiCallFlow::SUCCESS) {
    DLOG(INFO) << "Confirm success.";
    confirm_api_call_flow_.reset();
    current_register_operation_->CompleteRegistration();
  } else {
    // TODO(noamsml): Add detailed error message.
    LogRegisterErrorToWeb("Confirm error");
  }
}

void LocalDiscoveryUIHandler::DeviceChanged(
    bool added,
    const std::string& name,
    const DeviceDescription& description) {
  device_descriptions_[name] = description;

  base::StringValue service_name(name);
  base::DictionaryValue info;
  info.SetString("domain", description.address.host());
  info.SetInteger("port", description.address.port());
  std::string ip_addr_string;
  if (!description.ip_address.empty())
    ip_addr_string = net::IPAddressToString(description.ip_address);

  info.SetString("ip", ip_addr_string);
  info.SetString("lastSeen", "unknown");
  info.SetBoolean("registered", !description.id.empty());

  web_ui()->CallJavascriptFunction("local_discovery.onServiceUpdate",
                                   service_name, info);
}

void LocalDiscoveryUIHandler::DeviceRemoved(const std::string& name) {
  device_descriptions_.erase(name);
  scoped_ptr<base::Value> null_value(base::Value::CreateNullValue());
  base::StringValue name_value(name);

  web_ui()->CallJavascriptFunction("local_discovery.onServiceUpdate",
                                   name_value, *null_value);
}

void LocalDiscoveryUIHandler::LogRegisterErrorToWeb(const std::string& error) {
  base::StringValue error_value(error);
  web_ui()->CallJavascriptFunction("local_discovery.registrationFailed",
                                   error_value);
  DLOG(ERROR) << error;
}

void LocalDiscoveryUIHandler::LogRegisterDoneToWeb(const std::string& id) {
  base::StringValue id_value(id);
  web_ui()->CallJavascriptFunction("local_discovery.registrationSuccess",
                                   id_value);
  DLOG(INFO) << "Registered " << id;
}

void LocalDiscoveryUIHandler::LogInfoErrorToWeb(const std::string& error) {
  base::StringValue error_value(error);
  web_ui()->CallJavascriptFunction("local_discovery.infoFailed", error_value);
  LOG(ERROR) << error;
}

void LocalDiscoveryUIHandler::OnPrivetInfoDone(
    PrivetInfoOperation* operation,
    int http_code,
    const base::DictionaryValue* json_value) {
  if (http_code != net::HTTP_OK || !json_value) {
    LogInfoErrorToWeb(base::StringPrintf("HTTP error %d", http_code));
    return;
  }

  web_ui()->CallJavascriptFunction("local_discovery.renderInfo", *json_value);
}

void LocalDiscoveryUIHandler::OnCloudPrintAccountsResolved(
    const std::vector<std::string>& accounts,
    const std::string& xsrf_token) {
  xsrf_token_for_primary_user_ = xsrf_token;

  std::string sync_account = GetSyncAccount();
  base::ListValue accounts_annotated_list;

  if (!sync_account.empty()) {
    scoped_ptr<base::ListValue> account_annotated(new base::ListValue);
    account_annotated->AppendInteger(kAccountIndexUseOAuth2);
    account_annotated->AppendString(sync_account);
    accounts_annotated_list.Append(account_annotated.release());
  }

  int account_index = 0;
  for (std::vector<std::string>::const_iterator i = accounts.begin();
       i != accounts.end(); i++, account_index++) {
    if (*i == sync_account) continue;

    scoped_ptr<base::ListValue> account_annotated(new base::ListValue);
    account_annotated->AppendInteger(account_index);
    account_annotated->AppendString(*i);
    accounts_annotated_list.Append(account_annotated.release());
  }

  web_ui()->CallJavascriptFunction("local_discovery.requestUser",
                                   accounts_annotated_list);
}

void LocalDiscoveryUIHandler::OnXSRFTokenForSecondaryAccount(
    const GURL& automated_claim_url,
    const std::vector<std::string>& accounts,
    const std::string& xsrf_token) {
  StartCookieConfirmFlow(current_register_user_index_,
                         xsrf_token,
                         automated_claim_url);
}

void LocalDiscoveryUIHandler::SetIsVisible(bool visible) {
  if (visible != is_visible_) {
    g_num_visible += visible ? 1 : -1;
    is_visible_ = visible;
  }
}

std::string LocalDiscoveryUIHandler::GetSyncAccount() {
  Profile* profile = Profile::FromWebUI(web_ui());
  SigninManagerBase* signin_manager =
      SigninManagerFactory::GetForProfileIfExists(profile);

  if (!signin_manager) {
    return "";
  }

  return signin_manager->GetAuthenticatedUsername();
}

const std::string& LocalDiscoveryUIHandler::GetCloudPrintBaseUrl(
    const std::string& device_name) {
  return device_descriptions_[device_name].url;
}

void LocalDiscoveryUIHandler::StartCookieConfirmFlow(
    int user_index,
    const std::string& xsrf_token,
    const GURL& automated_claim_url) {
  confirm_api_call_flow_.reset(new PrivetConfirmApiCallFlow(
      Profile::FromWebUI(web_ui())->GetRequestContext(),
      user_index,
      xsrf_token,
      automated_claim_url,
      base::Bind(&LocalDiscoveryUIHandler::OnConfirmDone,
                 base::Unretained(this))));

  confirm_api_call_flow_->Start();
}

}  // namespace local_discovery
