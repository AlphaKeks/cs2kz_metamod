#include <cstdio>
#include <string>

#include "utils/json.h"

#include "kz/kz.h"
#include "kz/language/kz_language.h"
#include "error.h"
#include "kz/option/kz_option.h"
#include "kz_global.h"
#include "players.h"
#include "utils/ctimer.h"
#include "utils/http.h"
#include "version.h"

std::optional<KZ::API::Map> KZGlobalService::currentMap = std::nullopt;
const char *KZGlobalService::apiURL = "https://api.cs2kz.org";
std::atomic<bool> KZGlobalService::isHealthy = false;
std::atomic<bool> KZGlobalService::authTimerInitialized = false;
std::optional<std::string> KZGlobalService::apiKey = std::nullopt;
std::optional<std::string> KZGlobalService::authPayload = std::nullopt;
std::optional<std::string> KZGlobalService::apiToken = std::nullopt;

extern ISteamHTTP *g_pHTTP;

void KZGlobalService::Init()
{
	META_CONPRINTF("[KZ::Global] Initializing API connection...\n");

	KZGlobalService::apiURL = KZOptionService::GetOptionStr("apiURL", "https://api.cs2kz.org");
	META_CONPRINTF("[KZ::Global] Registered API URL: `%s`\n", KZGlobalService::apiURL);

	const char *apiKey = KZOptionService::GetOptionStr("apiKey", "");

	if (apiKey[0] == '\0')
	{
		META_CONPRINTF("[KZ::Global] No API key found! Will not attempt to authenticate.\n");
	}
	else
	{
		META_CONPRINTF("[KZ::Global] Loaded API key from config. Starting heartbeat...\n");
		KZGlobalService::apiKey = apiKey;
	}

	StartTimer(Heartbeat, true, true);
}

f64 KZGlobalService::Heartbeat()
{
	HTTP::Request request(HTTP::Method::GET, apiURL);

	request.Send([](HTTP::Response response) {
		switch (response.status)
		{
			case 0:
			{
				META_CONPRINTF("[KZ::Global] API is unreachable.\n");
				isHealthy = false;
				return;
			}

			case 200:
			{
				auto body = response.Body();

				if (!body)
				{
					META_CONPRINTF("[KZ::Global] API healthcheck did not contain a body\n");
					isHealthy = false;
					return;
				}

				META_CONPRINTF("[KZ::Global] API is healthy %s\n", body->c_str());
				isHealthy = true;

				if (!authTimerInitialized)
				{
					META_CONPRINTF("[KZ::Global] Initializing auth flow...\n");
					StartTimer(Auth, true, true);
					authTimerInitialized = true;
				}

				break;
			}

			default:
			{
				META_CONPRINTF("[KZ::Global] API healthcheck failed with status %d: `%s`\n", response.status, response.Body().value_or("").c_str());
				isHealthy = false;
				return;
			}
		}
	});

	return heartbeatInterval;
}

f64 KZGlobalService::Auth()
{
	if (!apiKey.has_value())
	{
		META_CONPRINTF("[KZ::Global] No API key found, can't authenticate.\n");
		return -1.0;
	}

	if (!authPayload)
	{
		json requestBody;
		requestBody["refresh_key"] = apiKey.value();
		requestBody["plugin_version"] = VERSION_STRING;

		authPayload = requestBody.dump();
	}

	const std::string requestURL = std::string(apiURL) + "/servers/key";

	HTTP::Request request(HTTP::Method::POST, requestURL);

	request.SetBody(authPayload.value());

	request.Send([](HTTP::Response response) {
		switch (response.status)
		{
			case 0:
			{
				META_CONPRINTF("[KZ::Global] Failed to request access token.\n");
				return;
			}

			case 201:
			{
				auto rawBody = response.Body();

				if (!rawBody)
				{
					META_CONPRINTF("[KZ::Global] Acces token response has no body\n");
					return;
				}

				const json responseBody = json::parse(rawBody.value());

				if (!responseBody.is_object() || !responseBody.contains("access_key"))
				{
					META_CONPRINTF("[KZ::Global] Access token response has unexpected shape: `%s`\n", rawBody->c_str());
					;
					return;
				}

				apiToken = responseBody["access_key"];
				META_CONPRINTF("[KZ::Global] Fetched access key `%s`\n", apiToken->c_str());
				break;
			}

			default:
			{
				KZ::API::Error error(response.status, response.Body().value_or(""));

				META_CONPRINTF("[KZ::Global] Fetching access key failed with status %d: %s\n", error.status, error.message.c_str());

				if (!error.details.is_null())
				{
					META_CONPRINTF("     Details: `%s`\n", error.details.dump().c_str());
				}

				return;
			}
		}
	});

	return authInterval;
}

bool FetchPlayerImpl(const char *url, KZGlobalService::Callback<std::optional<KZ::API::Player>> onSuccess,
					 KZGlobalService::Callback<KZ::API::Error> onError)
{
	if (!KZGlobalService::IsHealthy())
	{
		META_CONPRINTF("[KZ::Global] Cannot fetch player (API is currently not healthy).\n", url);
		onError({503, "unreachable"});
		return false;
	}

	HTTP::Request request(HTTP::Method::GET, url);

	request.Send([onSuccess, onError](HTTP::Response response) {
		switch (response.status)
		{
			case 0:
			{
				META_CONPRINTF("[KZ::Global] Failed to make HTTP request.\n");
				return;
			}

			case 200:
			{
				auto body = response.Body();

				if (!body)
				{
					META_CONPRINTF("[KZ::Global] Player response has no body\n");
					return;
				}

				auto json = json::parse(body.value());
				KZ::API::Player player;

				if (auto error = KZ::API::Player::Deserialize(json, player))
				{
					META_CONPRINTF("[KZ::Global] Failed to parse player: %s\n", error->reason.c_str());
				}
				else
				{
					onSuccess(player);
				}

				break;
			}

			case 404:
			{
				onSuccess(std::nullopt);
				break;
			}

			default:
			{
				KZ::API::Error error(response.status, response.Body().value_or(""));
				onError(error);
				return;
			}
		}
	});

	return true;
}

bool KZGlobalService::FetchPlayer(const char *name, Callback<std::optional<KZ::API::Player>> onSuccess, Callback<KZ::API::Error> onError)
{
	CUtlString url;
	url.Format("%s/players/%s", apiURL, name);
	return FetchPlayerImpl(url.Get(), onSuccess, onError);
}

bool KZGlobalService::FetchPlayer(u64 steamID, Callback<std::optional<KZ::API::Player>> onSuccess, Callback<KZ::API::Error> onError)
{
	CUtlString url;
	url.Format("%s/players/%llu", apiURL, steamID);
	return FetchPlayerImpl(url.Get(), onSuccess, onError);
}

bool KZGlobalService::RegisterPlayer(KZPlayer *player, Callback<KZ::API::Error> onError)
{
	if (!IsHealthy())
	{
		META_CONPRINTF("[KZ::Global] Cannot register player (API is currently not healthy).\n");
		onError({503, "unreachable"});
		return false;
	}

	if (!IsAuthenticated())
	{
		META_CONPRINTF("[KZ::Global] Cannot register player (not authenticated with API).\n");
		onError({401, "server is not global"});
		return false;
	}

	CUtlString url;
	url.Format("%s/players", apiURL);

	auto newPlayer = KZ::API::NewPlayer {player->GetName(), player->GetSteamId64(), player->GetIpAddress()};
	json requestBody = newPlayer.Serialize();

	HTTP::Request request(HTTP::Method::POST, url.Get());

	request.SetHeader("Authorization", (std::string("Bearer ") + apiToken.value()));
	request.SetBody(requestBody.dump());

	request.Send([player, onError](HTTP::Response response) {
		switch (response.status)
		{
			case 0:
			{
				META_CONPRINTF("[KZ::Global] Failed to make HTTP request.\n");
				break;
			}

			case 201:
			{
				auto onSuccess = [player](std::optional<KZ::API::Player> info) {
					if (!info)
					{
						player->languageService->PrintChat(true, false, "Player not found after registration");
						return;
					}

					player->languageService->PrintChat(true, false, "Display Hello", info->name.c_str());
					player->info = info.value();
				};

				auto onError = [player](KZ::API::Error error) {
					player->languageService->PrintError(error);
				};

				KZGlobalService::FetchPlayer(player->GetSteamId64(), onSuccess, onError);

				break;
			}

			default:
			{
				KZ::API::Error error(response.status, response.Body().value_or(""));
				onError(error);
				return;
			}
		}
	});

	return true;
}

bool KZGlobalService::UpdatePlayer(KZPlayer *player, Callback<std::optional<KZ::API::Error>> onError)
{
	if (!KZGlobalService::IsHealthy())
	{
		META_CONPRINTF("[KZ::Global] Cannot fetch map (API is currently not healthy).\n");
		onError(std::make_optional<KZ::API::Error>({503, "unreachable"}));
		return false;
	}

	CUtlString url;
	url.Format("%s/players/%llu", apiURL, player->GetSteamId64());

	const KZ::API::PlayerUpdate playerUpdate = {player->GetName(), player->GetIpAddress(), json::object(), player->session};
	const json requestBody = playerUpdate.Serialize();

	HTTP::Request request(HTTP::Method::PATCH, url.Get());

	request.SetHeader("Authorization", (std::string("Bearer ") + apiToken.value()));
	request.SetBody(requestBody.dump());

	META_CONPRINTF("[KZ::Global] updating player at `%s` with `%s`\n", url.Get(), requestBody.dump().c_str());
	request.Send([player, onError](HTTP::Response response) {
		switch (response.status)
		{
			case 0:
			{
				META_CONPRINTF("[KZ::Global] Failed to make HTTP request.\n");
				return;
			}

			case 204:
			{
				onError(std::nullopt);
				break;
			}

			default:
			{
				KZ::API::Error error(response.status, response.Body().value_or(""));
				onError(error);
				return;
			}
		}
	});

	return true;
}

bool FetchMapImpl(const char *url, KZGlobalService::Callback<std::optional<KZ::API::Map>> onSuccess,
				  KZGlobalService::Callback<KZ::API::Error> onError)
{
	if (!KZGlobalService::IsHealthy())
	{
		META_CONPRINTF("[KZ::Global] Cannot fetch map (API is currently not healthy).\n");
		onError({503, "unreachable"});
		return false;
	}

	HTTP::Request request(HTTP::Method::GET, url);

	request.Send([onSuccess, onError](HTTP::Response response) {
		switch (response.status)
		{
			case 0:
			{
				META_CONPRINTF("[KZ::Global] Failed to make HTTP request.\n");
				return;
			}

			case 200:
			{
				auto body = response.Body();

				if (!body)
				{
					META_CONPRINTF("[KZ::Global] Map response has no body\n");
					return;
				}

				auto json = json::parse(body.value());
				KZ::API::Map map;

				if (auto error = KZ::API::Map::Deserialize(json, map))
				{
					META_CONPRINTF("[KZ::Global] Failed to deserialize map: %s\n", error->reason.c_str());
				}
				else
				{
					onSuccess(map);
				}

				break;
			}

			case 404:
			{
				onSuccess(std::nullopt);
				break;
			}

			default:
			{
				KZ::API::Error error(response.status, response.Body().value_or(""));
				onError(error);
				return;
			}
		}
	});

	return true;
}

bool KZGlobalService::FetchMap(const char *name, Callback<std::optional<KZ::API::Map>> onSuccess, Callback<KZ::API::Error> onError)
{
	CUtlString url;
	url.Format("%s/maps/%s", apiURL, name);
	return FetchMapImpl(url.Get(), onSuccess, onError);
}

bool KZGlobalService::FetchMap(u16 id, Callback<std::optional<KZ::API::Map>> onSuccess, Callback<KZ::API::Error> onError)
{
	CUtlString url;
	url.Format("%s/maps/%d", apiURL, id);
	return FetchMapImpl(url.Get(), onSuccess, onError);
}
