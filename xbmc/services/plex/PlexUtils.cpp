/*
 *      Copyright (C) 2016-2017 Team MrMC
 *      https://github.com/MrMC
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with MrMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "PlexUtils.h"
#include "PlexServices.h"
#include "Application.h"
#include "ContextMenuManager.h"
#include "Util.h"
#include "URL.h"
#include "filesystem/StackDirectory.h"
#include "network/Network.h"
#include "utils/Base64URL.h"
#include "utils/log.h"
#include "utils/StringUtils.h"
#include "utils/SystemInfo.h"
#include "utils/URIUtils.h"
#include "utils/XMLUtils.h"
#include "filesystem/Directory.h"
#include "filesystem/File.h"
#include "filesystem/CurlFile.h"
#include "filesystem/ZipFile.h"
#include "settings/Settings.h"
#include "settings/MediaSettings.h"

#include "contrib/xml2json/xml2json.hpp"
#include "utils/JSONVariantParser.h"

#include "video/VideoInfoTag.h"
#include "video/windows/GUIWindowVideoBase.h"

#include "music/tags/MusicInfoTag.h"
#include "music/dialogs/GUIDialogSongInfo.h"
#include "music/dialogs/GUIDialogMusicInfo.h"
#include "guilib/GUIWindowManager.h"

static int  g_progressSec = 0;
static int  g_pingSec = 0;
static CFileItem m_curItem;
static MediaServicesPlayerState g_playbackState = MediaServicesPlayerState::stopped;


static void removeLeadingSlash(std::string &path)
{
  if (!path.empty() && (path[0] == '/'))
    StringUtils::TrimLeft(path, "/");
}

static const CVariant makeVariantArrayIfSingleItem(const CVariant &variant)
{
  CVariant variantArray(CVariant::VariantTypeArray);
  if (variant.isObject())
  {
    // variant could be an object (not array)
    // convert it to an array of one item
    variantArray.push_back(variant);
    return variantArray;
  }
  else if (variant.isArray())
  {
    return variant;
  }
  return CVariant::VariantTypeNull;
}
/*
static std::string makeImagePathUsing(CURL &curl, const std::string &path)
{
  std::string imagePath(path);
  std::string oldFileName(curl.GetFileName());
  removeLeadingSlash(imagePath);
  curl.SetFileName(imagePath);
  imagePath = curl.Get();
  curl.SetFileName(oldFileName);
  return imagePath;
}
*/

bool CPlexUtils::HasClients()
{
  return CPlexServices::GetInstance().HasClients();
}

void CPlexUtils::GetClientHosts(std::vector<std::string>& hosts)
{
  std::vector<CPlexClientPtr> clients;
  CPlexServices::GetInstance().GetClients(clients);
  for (const auto &client : clients)
    hosts.push_back(client->GetHost());
}

bool CPlexUtils::GetIdentity(CURL url, int timeout)
{
  // all (local and remote) plex server respond to identity
  XFILE::CCurlFile plex;
  plex.SetTimeout(timeout);
  plex.SetSilent(true);

  url.SetFileName(url.GetFileName() + "identity");
  std::string strResponse;
  if (plex.Get(url.Get(), strResponse))
  {
#if defined(PLEX_DEBUG_VERBOSE)
    CLog::Log(LOGDEBUG, "CPlexClient::GetIdentity() %s", strResponse.c_str());
#endif
    return true;
  }

  return false;
}

void CPlexUtils::GetDefaultHeaders(XFILE::CCurlFile *curl)
{
  curl->SetRequestHeader("Content-Type", "application/xml; charset=utf-8");
  curl->SetRequestHeader("Content-Length", "0");
  curl->SetRequestHeader("Connection", "Keep-Alive");
  curl->SetUserAgent(CSysInfo::GetUserAgent());
  curl->SetRequestHeader("X-Plex-Client-Identifier", CSettings::GetInstance().GetString(CSettings::SETTING_SERVICES_UUID));
  curl->SetRequestHeader("X-Plex-Product", "MrMC");
  curl->SetRequestHeader("X-Plex-Version", CSysInfo::GetVersionShort());
  std::string hostname;
  g_application.getNetwork().GetHostName(hostname);
  StringUtils::TrimRight(hostname, ".local");
  curl->SetRequestHeader("X-Plex-Model", CSysInfo::GetModelName());
  curl->SetRequestHeader("X-Plex-Device", CSysInfo::GetModelName());
  curl->SetRequestHeader("X-Plex-Device-Name", hostname);
  curl->SetRequestHeader("X-Plex-Platform", CSysInfo::GetOsName());
  curl->SetRequestHeader("X-Plex-Platform-Version", CSysInfo::GetOsVersion());
  curl->SetRequestHeader("Cache-Control", "no-cache");
  curl->SetRequestHeader("Pragma", "no-cache");
  curl->SetRequestHeader("Expires", "Sat, 26 Jul 1997 05:00:00 GMT");
}

void CPlexUtils::SetPlexItemProperties(CFileItem &item)
{
  CPlexClientPtr client = CPlexServices::GetInstance().FindClient(item.GetPath());
  SetPlexItemProperties(item, client);
}

void CPlexUtils::SetPlexItemProperties(CFileItem &item, const CPlexClientPtr &client)
{
  item.SetProperty("PlexItem", true);
  item.SetProperty("MediaServicesItem", true);
  if (!client)
    return;
  if (client->IsCloud())
    item.SetProperty("MediaServicesCloudItem", true);
  item.SetProperty("MediaServicesClientID", client->GetUuid());
  if (client->IsLocal())
    item.SetProperty("MediaServicesLocalItem", true);
}

void CPlexUtils::SetPlexRatingProperties(CFileItem &plexItem, const CVariant &item)
{
  // set rating (extract ratingtype from ratingimage)
  std::string ratingProvider = "";
  std::string ratingDelimiter = "://";
  std::string ratingImageValue = item["ratingImage"].asString();
  if (ratingImageValue != "")
  {
    std::string::size_type pos = ratingImageValue.find(ratingDelimiter);
    ratingProvider = ratingImageValue.substr(0, pos);
    std::string ratingImage = ratingImageValue.substr(pos + ratingDelimiter.length());
    plexItem.SetProperty("ratingImage", ratingImage);
    if (ratingProvider == "rottentomatoes")
      plexItem.SetProperty("RottenTomatoesRating", item["rating"].asFloat() * 10);
  }
  plexItem.GetVideoInfoTag()->SetRating(item["rating"].asFloat(), ratingProvider, true);
  
  // audience rating
  std::string audienceRatingImageValue = item["audienceRatingImage"].asString();
  if (audienceRatingImageValue != "")
  {
    std::string::size_type pos = audienceRatingImageValue.find(ratingDelimiter);
    ratingProvider = audienceRatingImageValue.substr(0, pos);
    std::string audienceRatingImage = audienceRatingImageValue.substr(pos + ratingDelimiter.length());
    plexItem.SetProperty("audienceRatingImage", audienceRatingImage);
    plexItem.SetProperty("audienceRating", item["audienceRating"].asString());
    if (ratingProvider == "rottentomatoes")
      plexItem.SetProperty("RottenTomatoesAudienceRating", item["audienceRating"].asFloat() * 10);
    ratingProvider += "audience";
    plexItem.GetVideoInfoTag()->SetRating(item["audienceRating"].asFloat(), ratingProvider, false);
  }
  
  // User rating
  if (item["userRating"].asString() != "") {
    plexItem.GetVideoInfoTag()->SetUserrating(item["userRating"].asInteger());
  }
  
  // MPAA rating
  plexItem.GetVideoInfoTag()->m_strMPAARating = item["contentRating"].asString();
}

#pragma mark - Plex Server Utils
void CPlexUtils::SetWatched(CFileItem &item)
{
  // if we are Plex music, do not report
  if (item.IsAudio())
    return;

  std::string id = item.GetMediaServiceId();
  std::string url = item.GetPath();
  if (URIUtils::IsStack(url))
    url = XFILE::CStackDirectory::GetFirstStackedFile(url);
  else
    url   = URIUtils::GetParentPath(url);
  if (StringUtils::StartsWithNoCase(url, "plex://"))
      url = Base64URL::Decode(URIUtils::GetFileName(item.GetPath()));

  std::string filename = StringUtils::Format(":/scrobble?identifier=com.plexapp.plugins.library&key=%s", id.c_str());
  ReportToServer(url, filename);
}

void CPlexUtils::SetUnWatched(CFileItem &item)
{
  // if we are Plex music, do not report
  if (item.IsAudio())
    return;

  std::string id = item.GetMediaServiceId();
  std::string url = item.GetPath();
  if (URIUtils::IsStack(url))
    url = XFILE::CStackDirectory::GetFirstStackedFile(url);
  else
    url   = URIUtils::GetParentPath(url);
  if (StringUtils::StartsWithNoCase(url, "plex://"))
    url = Base64URL::Decode(URIUtils::GetFileName(item.GetPath()));

  std::string filename = StringUtils::Format(":/unscrobble?identifier=com.plexapp.plugins.library&key=%s", id.c_str());
  ReportToServer(url, filename);
}

void CPlexUtils::ReportProgress(CFileItem &item, double currentSeconds)
{
  // if we are Plex music, do not report
  if (item.IsAudio())
    return;
  
  // we get called from Application.cpp every 500ms
  if ((g_playbackState == MediaServicesPlayerState::stopped || g_progressSec == 0 || g_progressSec > 120))
  {
    g_progressSec = 0;

    std::string status;
    if (g_playbackState == MediaServicesPlayerState::playing )
      status = "playing";
    else if (g_playbackState == MediaServicesPlayerState::paused )
      status = "paused";
    else if (g_playbackState == MediaServicesPlayerState::stopped)
      status = "stopped";

    if (!status.empty())
    {
      std::string url = item.GetPath();
      if (URIUtils::IsStack(url))
        url = XFILE::CStackDirectory::GetFirstStackedFile(url);
      else
      {
        CURL url1(item.GetPath());
        CURL url2(URIUtils::GetParentPath(url));
        CURL url3(url2.GetWithoutFilename());
        url3.SetProtocolOptions(url1.GetProtocolOptions());
        url = url3.Get();
      }
      if (StringUtils::StartsWithNoCase(url, "plex://"))
        url = Base64URL::Decode(URIUtils::GetFileName(item.GetPath()));

      std::string id    = item.GetMediaServiceId();
      int totalSeconds  = item.GetVideoInfoTag()->m_resumePoint.totalTimeInSeconds;

      std::string filename = StringUtils::Format(":/timeline?ratingKey=%s&",id.c_str());
      filename = filename + "key=%2Flibrary%2Fmetadata%2F" +
        StringUtils::Format("%s&state=%s&time=%i&duration=%i", id.c_str(), status.c_str(),
          (int)currentSeconds * 1000, totalSeconds * 1000);

      ReportToServer(url, filename);
      //CLog::Log(LOGDEBUG, "CPlexUtils::ReportProgress %s", filename.c_str());
    }
    if (g_playbackState == MediaServicesPlayerState::stopped &&
        item.GetProperty("PlexTranscoder").asBoolean())
    {
      StopTranscode(item);
    }
    
  }
  g_progressSec++;
  
  if (g_playbackState == MediaServicesPlayerState::paused &&
      item.GetProperty("PlexTranscoder").asBoolean())
  {
    // if item is on a local server, we ping the server every 10 seconds... otherwise every 30 sec
    int pingSec = item.GetProperty("MediaServicesLocalItem").asBoolean() ? 20 : 60;
    if (g_pingSec > pingSec)
    {
      PingTranscoder(item);
      g_pingSec = 0;
    }
    g_pingSec++;
  }
  
}

void CPlexUtils::SetPlayState(MediaServicesPlayerState state)
{
  g_pingSec = 0;
  g_progressSec = 0;
  g_playbackState = state;
}

bool CPlexUtils::GetPlexMediaTotals(MediaServicesMediaCount &totals)
{
  // totals might or might not be reset to zero so add to it
  std::vector<CPlexClientPtr> clients;
  CPlexServices::GetInstance().GetClients(clients);
  for (const auto &client : clients)
  {
    /// Only do this for "owned" servers, getting totals from large servers can take up to 1 minute. setting for this maybe?
    if (client->GetOwned() == "1")
    {
      std::vector<PlexSectionsContent> contents;
      // movie totals
      contents = client->GetMovieContent();
      for (const auto &content : contents)
      {
        TiXmlDocument xml;
        CURL curl(client->GetUrl());
        curl.SetProtocol(client->GetProtocol());
        curl.SetFileName(content.section + "/all?type=1&unwatched=1");
        curl.SetProtocolOption("X-Plex-Container-Start", "0");
        curl.SetProtocolOption("X-Plex-Container-Size", "0");
        // get movie unwatched totals
        xml = GetPlexXML(curl.Get());
        totals.iMovieUnwatched += ParsePlexMediaXML(xml);
        // get movie totals
        curl.SetFileName(content.section + "/all?type=1");
        xml = GetPlexXML(curl.Get());
        totals.iMovieTotal +=ParsePlexMediaXML(xml);
      }
      // Show Totals
      contents = client->GetTvContent();
      for (const auto &content : contents)
      {
        TiXmlDocument xml;
        CURL curl(client->GetUrl());
        curl.SetProtocol(client->GetProtocol());
        curl.SetFileName(content.section + "/all?type=4&unwatched=1");
        curl.SetProtocolOption("X-Plex-Container-Start", "0");
        curl.SetProtocolOption("X-Plex-Container-Size", "0");
        // get episode unwatched totals
        xml = GetPlexXML(curl.Get());
        totals.iEpisodeUnwatched += ParsePlexMediaXML(xml);
        // get episode totals
        curl.SetFileName(content.section + "/all?type=4");
        xml = GetPlexXML(curl.Get());
        totals.iEpisodeTotal += ParsePlexMediaXML(xml);
        // get show totals
        curl.SetFileName(content.section + "/all?type=2");
        xml = GetPlexXML(curl.Get());
        totals.iShowTotal += ParsePlexMediaXML(xml);
        // get show unwatched totals
        curl.SetFileName(content.section + "/all?type=2&unwatched=1");
        xml = GetPlexXML(curl.Get());
        totals.iShowUnwatched += ParsePlexMediaXML(xml);
      }
      // Music Totals
      contents = client->GetArtistContent();
      for (const auto &content : contents)
      {
        TiXmlDocument xml;
        CURL curl(client->GetUrl());
        curl.SetProtocol(client->GetProtocol());
        curl.SetFileName(content.section + "/all?type=8");
        curl.SetProtocolOption("X-Plex-Container-Start", "0");
        curl.SetProtocolOption("X-Plex-Container-Size", "0");
        // get artist totals
        xml = GetPlexXML(curl.Get());
        totals.iMusicArtist += ParsePlexMediaXML(xml);
        // get Album totals
        curl.SetFileName(content.section + "/all?type=9");
        xml = GetPlexXML(curl.Get());
        totals.iMusicAlbums += ParsePlexMediaXML(xml);
        // get Song totals
        curl.SetFileName(content.section + "/all?type=10");
        xml = GetPlexXML(curl.Get());
        totals.iMusicSongs += ParsePlexMediaXML(xml);
      }
    }
  }
  return true;
}

bool CPlexUtils::DeletePlexMedia(CFileItem &item)
{
//  library/metadata/
  CURL url2(item.GetURL());
  url2.SetFileName("library/metadata/" + item.GetMediaServiceId());
  std::string strXML;
  XFILE::CCurlFile plex;
  CPlexUtils::GetDefaultHeaders(&plex);
  std::string data;
  std::string response;
  plex.Delete(url2.Get(),data,response);
  return true;
}

#pragma mark - Plex Playlists
bool CPlexUtils::GetPlexVideoPlaylistItems(CFileItemList &items, const std::string url)
{
  bool rtn = false;
  CURL curl(url);
  CVariant variant = GetPlexCVariant(curl.Get());
  if (!variant.isNull() && variant.isObject() && variant.isMember("MediaContainer"))
  {
    CVariant variantVideo = variant["MediaContainer"]["Video"];
    for (auto variantIt = variantVideo.begin_array(); variantIt != variantVideo.end_array(); ++variantIt)
    {
      const auto item = *variantIt;
      if (item.isMember("parentIndex"))
      {
        int season = item["parentIndex"].asInteger();
        rtn = ParsePlexVideos(items, curl, item, MediaTypeEpisode, false, season);
      }
      else
        rtn = ParsePlexVideos(items, curl, item, MediaTypeMovie, false);
    }
    if (rtn)
    {
      items.SetLabel(variant["MediaContainer"]["title"].asString());
    }
  }
  return rtn;
}

bool CPlexUtils::GetPlexMusicPlaylistItems(CFileItemList &items, const std::string url)
{
  // for clarity we call GetPlexMusicPlaylistItems which is the same as GetPlexSongs.
  // where are music videos?
  return GetPlexSongs(items,url);;
}

#pragma mark - Plex Continue Watching
bool CPlexUtils::GetPlexContinueWatching(CFileItemList &items, const std::string url)
{
  bool rtn = false;
  CURL curl(url);
  CVariant variant = GetPlexCVariant(curl.Get());
  if (!variant.isNull() && variant.isObject() && variant.isMember("MediaContainer"))
  {
    CVariant variantVideo = variant["MediaContainer"]["Video"];
    for (auto variantIt = variantVideo.begin_array(); variantIt != variantVideo.end_array(); ++variantIt)
    {
      const auto item = *variantIt;
      if (item.isMember("parentIndex"))
      {
        int season = item["parentIndex"].asInteger();
        rtn = ParsePlexVideos(items, curl, item, MediaTypeEpisode, false, season);
      }
      else
        rtn = ParsePlexVideos(items, curl, item, MediaTypeMovie, false);
    }
    if (rtn)
    {
      items.SetLabel(variant["MediaContainer"]["title2"].asString());
    }
  }
  return rtn;
}

#pragma mark - Plex Recently Added and InProgress
bool CPlexUtils::GetPlexRecentlyAddedEpisodes(CFileItemList &items, const std::string url, int limit, bool watched)
{
  bool rtn = false;
  CURL curl(url);
  // special case below, we can reuse base64 string
  std::string fileName = curl.GetFileName();
  StringUtils::TrimRight(fileName, "all");
  curl.SetFileName(fileName + "recentlyAdded");
  if (!watched)
    curl.SetProtocolOption("unwatched","1");
  curl.SetProtocolOptions(curl.GetProtocolOptions() + StringUtils::Format("&X-Plex-Container-Start=0&X-Plex-Container-Size=%i", limit));
  
  CVariant variant = GetPlexCVariant(curl.Get());
  if (!variant.isNull() && variant.isObject() && variant.isMember("MediaContainer"))
  {
    rtn = ParsePlexVideos(items, curl, variant["MediaContainer"]["Video"], MediaTypeEpisode, false);
    if (rtn)
    {
      items.SetLabel(variant["MediaContainer"]["title2"].asString());
    }
  }

  return rtn;
}

bool CPlexUtils::GetPlexInProgressShows(CFileItemList &items, const std::string url, int limit)
{
  bool rtn = false;
  CURL curl(url);
  std::string strXML;
  // special case below, we can reuse base64 string
  std::string fileName = curl.GetFileName();
  StringUtils::TrimRight(fileName, "all");
  curl.SetFileName(fileName + "onDeck");
  curl.SetProtocolOptions(curl.GetProtocolOptions() + StringUtils::Format("&X-Plex-Container-Start=0&X-Plex-Container-Size=%i", limit));

  CVariant variant = GetPlexCVariant(curl.Get());
  if (!variant.isNull() && variant.isObject() && variant.isMember("MediaContainer"))
  {
    rtn = ParsePlexVideos(items, curl, variant["MediaContainer"]["Video"], MediaTypeEpisode, false);
    if (rtn)
    {
      items.SetLabel(variant["MediaContainer"]["title2"].asString());
    }
  }

  return rtn;
}

bool CPlexUtils::GetPlexRecentlyAddedMovies(CFileItemList &items, const std::string url, int limit, bool watched)
{
  bool rtn = false;
  CURL curl(url);
  // special case below, we can reuse base64 string
  std::string fileName = curl.GetFileName();
  StringUtils::TrimRight(fileName, "all");
  curl.SetFileName(fileName + "recentlyAdded");
  if (!watched)
    curl.SetOption("unwatched","1");
  curl.SetProtocolOptions(curl.GetProtocolOptions() + StringUtils::Format("&X-Plex-Container-Start=0&X-Plex-Container-Size=%i", limit));
  
  CVariant variant = GetPlexCVariant(curl.Get());
  if (!variant.isNull() && variant.isObject() && variant.isMember("MediaContainer"))
  {
    rtn = ParsePlexVideos(items, curl, variant["MediaContainer"]["Video"], MediaTypeMovie, false);
    if (rtn)
    {
      items.SetLabel(variant["MediaContainer"]["title2"].asString());
    }
  }

  return rtn;
}

bool CPlexUtils::GetPlexInProgressMovies(CFileItemList &items, const std::string url, int limit)
{
  bool rtn = false;
  CURL curl(url);
  // special case below, we can reuse base64 string
  std::string fileName = curl.GetFileName();
  if (StringUtils::Replace(fileName, "/all" , "") > 0)
  {
    fileName = fileName + "/onDeck";
  }
  curl.SetFileName(fileName);
  curl.SetProtocolOptions(curl.GetProtocolOptions() + StringUtils::Format("&X-Plex-Container-Start=0&X-Plex-Container-Size=%i", limit));

  CVariant variant = GetPlexCVariant(curl.Get());
  if (!variant.isNull() && variant.isObject() && variant.isMember("MediaContainer"))
  {
    rtn = ParsePlexVideos(items, curl, variant["MediaContainer"]["Video"], MediaTypeMovie, false);
    if (rtn)
    {
      items.SetLabel(variant["MediaContainer"]["title2"].asString());
    }
  }

  return rtn;
}

bool CPlexUtils::GetAllPlexRecentlyAddedMoviesAndShows(CFileItemList &items, bool tvShow, bool unWatched)
{
  bool rtn = false;
  if (CPlexServices::GetInstance().HasClients())
  {
    CFileItemList plexItems;
    int limitTo = CSettings::GetInstance().GetInt(CSettings::SETTING_SERVICES_PLEXLIMITHOMETO);
    if (limitTo < 2)
      return false;
    //look through all plex clients and pull recently added for each library section
    std::vector<CPlexClientPtr> clients;
    CPlexServices::GetInstance().GetClients(clients);
    for (const auto &client : clients)
    {
      if (limitTo == 2 && !client->IsOwned())
        continue;
      CURL curl(client->GetUrl());
      curl.SetProtocol(client->GetProtocol());
      curl.SetFileName(curl.GetFileName() + "hubs/home/");
      
      if (tvShow)
      {
        curl.SetProtocolOption("type","2");
        rtn = GetPlexRecentlyAddedEpisodes(plexItems, curl.Get(), 10, unWatched);
      }
      else
      {
        curl.SetProtocolOption("type","1");
        rtn = GetPlexRecentlyAddedMovies(plexItems, curl.Get(), 10, unWatched);
      }
      for (int item = 0; item < plexItems.Size(); ++item)
        CPlexUtils::SetPlexItemProperties(*plexItems[item], client);

      SetPlexItemProperties(plexItems);
      items.Append(plexItems);
      items.ClearSortState();
      items.Sort(SortByLastPlayed, SortOrderDescending);
      plexItems.ClearItems();
    }
  }

  return rtn;
}

bool CPlexUtils::GetAllPlexInProgress(CFileItemList &items, bool tvShow)
{
  if (CPlexServices::GetInstance().HasClients())
  {
    CFileItemList plexItems;
    int limitTo = CSettings::GetInstance().GetInt(CSettings::SETTING_SERVICES_PLEXLIMITHOMETO);
    if (limitTo < 2)
      return false;
    //look through all plex clients and pull recently added for each library section
    std::vector<CPlexClientPtr> clients;
    CPlexServices::GetInstance().GetClients(clients);
    for (const auto &client : clients)
    {
      if (limitTo == 2 && !client->IsOwned())
        continue;
      
      CURL curl(client->GetUrl());
      curl.SetProtocol(client->GetProtocol());
      curl.SetFileName(curl.GetFileName() + "library/");
      
      if (tvShow)
      {
        curl.SetProtocolOption("type","2");
        GetPlexInProgressShows(plexItems, curl.Get(), 10);
      }
      else
      {
        curl.SetProtocolOption("type","1");
        GetPlexInProgressMovies(plexItems, curl.Get(), 10);
      }
      for (int item = 0; item < plexItems.Size(); ++item)
        CPlexUtils::SetPlexItemProperties(*plexItems[item], client);

      SetPlexItemProperties(plexItems);
      items.Append(plexItems);
      items.ClearSortState();
      items.Sort(SortByLastPlayed, SortOrderDescending);
      plexItems.ClearItems();
    }
  }

  return items.Size() > 0;
}

bool CPlexUtils::GetPlexFilter(CFileItemList &items, std::string url, std::string parentPath, std::string filter)
{
  bool rtn = false;
  CVariant variant = GetPlexCVariant(url, filter);
  if (!variant.isNull() && variant.isObject() && variant.isMember("MediaContainer"))
  {
    CVariant directory(variant["MediaContainer"]["Directory"]);
    std::string title2 = variant["MediaContainer"]["title2"].asString();
    if (!directory.isNull())
    {
      const CVariant variantMedia = makeVariantArrayIfSingleItem(directory);
      for (auto variantIt = variantMedia.begin_array(); variantIt != variantMedia.end_array(); ++variantIt)
      {
        if (*variantIt != CVariant::VariantTypeNull)
        {
          rtn = true;
          CURL plex(url);
          std::string title = (*variantIt)["title"].asString();
          std::string key = (*variantIt)["key"].asString();
          std::string fastKey = (*variantIt)["fastKey"].asString();
          CFileItemPtr pItem(new CFileItem(title));
          pItem->m_bIsFolder = true;
          pItem->m_bIsShareOrDrive = false;
          if (fastKey.empty())
          {
            plex.SetFileName(plex.GetFileName() + "/" + key);
          }
          else
          {
            removeLeadingSlash(fastKey);
            std::vector<std::string> split = StringUtils::Split(fastKey, "?");
            std::string value = StringUtils::Split(split[1], "=")[0];
            plex.SetFileName(split[0]);
            plex.SetProtocolOption(value, key);
          }
          pItem->SetPath(parentPath + Base64URL::Encode(plex.Get()));
          pItem->SetLabel(title);
          pItem->SetProperty("SkipLocalArt", true);
          SetPlexItemProperties(*pItem);
          items.Add(pItem);
        }
      }
    }
    StringUtils::ToCapitalize(title2);
    items.SetLabel(title2);
  }

  return rtn;
}

bool CPlexUtils::GetPlexFilters(CFileItemList &items, std::string url, std::string parentPath)
{
  bool rtn = false;
  CVariant variant = GetPlexCVariant(url);
  if (!variant.isNull() && variant.isObject() && variant.isMember("MediaContainer"))
  {
    CURL parentFile(parentPath);
    const CVariant directory(variant["MediaContainer"]["Directory"]);
    if (!directory.isNull())
    {
      for (auto variantIt = directory.begin_array(); variantIt != directory.end_array(); ++variantIt)
      {
        if (*variantIt != CVariant::VariantTypeNull)
        {
          rtn = true;
          std::string title = (*variantIt)["title"].asString();
          std::string key = (*variantIt)["key"].asString();
          bool secondary = (*variantIt)["secondary"].asBoolean();
          bool search = (*variantIt)["search"].asBoolean();
          if (search)
            break;
          CFileItemPtr pItem(new CFileItem(title));
          pItem->m_bIsFolder = true;
          pItem->m_bIsShareOrDrive = false;
          CURL plex(url);
          // special case below, we can reuse base64 string
          std::string fileName = plex.GetFileName();
          StringUtils::TrimRight(fileName, "all");
          if (secondary)
          {
            plex.SetFileName(plex.GetFileName() + key);
            pItem->SetPath(parentPath + Base64URL::Encode(plex.Get()));
          }
          else
          {
            std::string path = "titles/";
            if (parentFile.GetHostName() == "tvshows" &&
                (key == "recentlyAdded" || key == "recentlyViewed" || key == "newest"))
              path = "seasons/";
            
            plex.SetFileName(fileName + key);
            pItem->SetPath(parentPath + path + Base64URL::Encode(plex.Get()));
          }
          pItem->SetLabel(title);
          pItem->SetProperty("SkipLocalArt", true);
          SetPlexItemProperties(*pItem);
          items.Add(pItem);
        }
      }
    }
  }
  
  return rtn;
}

bool CPlexUtils::GetPlexMusicFilters(CFileItemList &items, std::string url, std::string parentPath)
{
  bool rtn = false;
  CVariant variant = GetPlexCVariant(url);
  if (!variant.isNull() && variant.isObject() && variant.isMember("MediaContainer"))
  {
    std::string title2 = variant["MediaContainer"]["title2"].asString();
    std::string librarySectionID = variant["MediaContainer"]["librarySectionID"].asString();
    const CVariant directory(variant["MediaContainer"]["Directory"]);
    if (!directory.isNull())
    {
      for (auto variantIt = directory.begin_array(); variantIt != directory.end_array(); ++variantIt)
      {
        if (*variantIt != CVariant::VariantTypeNull)
        {
          rtn = true;
          std::string title = (*variantIt)["title"].asString();
          std::string type = (*variantIt)["type"].asString();
          std::string key = (*variantIt)["key"].asString();
          std::string fastKey = (*variantIt)["fastKey"].asString();
          bool secondary = (*variantIt)["secondary"].asBoolean();
          bool search = (*variantIt)["search"].asBoolean();
          if (search)
            break;
          CFileItemPtr pItem(new CFileItem(title));
          pItem->m_bIsFolder = true;
          pItem->m_bIsShareOrDrive = false;
          CURL plex(url);
          // special case below, we can reuse base64 string
          std::string fileName = plex.GetFileName();
          StringUtils::TrimRight(fileName, "all");
          if (secondary)
          {
            plex.SetFileName(plex.GetFileName() + key);
            pItem->SetPath("plex://music/filters/" + Base64URL::Encode(plex.Get()));
          }
          else
          {
            std::string path = "albums/";

            if (key == "all")
            {
              path = "artists/";
            }
            else if (key == "folder")
            {
              break;
//              path = "folder/";
            }
            if (type == "genre")
            {
              path = "artists/";
              plex.SetProtocolOption("genre", key);
              removeLeadingSlash(fastKey);
              std::vector<std::string> split = StringUtils::Split(fastKey, "?");
              fileName = split[0];
              key = "";
            }

            StringUtils::TrimRight(fileName, "/");
            plex.SetFileName(fileName + "/" + key);
            pItem->SetPath(parentPath + path + Base64URL::Encode(plex.Get()));
          }
          pItem->SetLabel(title);
          pItem->SetProperty("SkipLocalArt", true);
          SetPlexItemProperties(*pItem);
          items.Add(pItem);
        }
      }
    }
    if (title2.empty())
      items.SetLabel(g_localizeStrings.Get(2));
    else
      items.SetLabel(title2);
  }
  return rtn;
}

bool CPlexUtils::GetPlexMusicFilter(CFileItemList &items, std::string url, std::string parentPath, std::string filter)
{
  bool rtn = false;
  CVariant variant = GetPlexCVariant(url, filter);
  std::string librarySectionID = variant["MediaContainer"]["librarySectionID"].asString();
  if (!variant.isNull() && variant.isObject() && variant.isMember("MediaContainer"))
  {
    CVariant directory(variant["MediaContainer"]["Directory"]);
    std::string title2 = variant["MediaContainer"]["title2"].asString();
    if (!directory.isNull())
    {
      for (auto variantIt = directory.begin_array(); variantIt != directory.end_array(); ++variantIt)
      {
        if (*variantIt != CVariant::VariantTypeNull)
        {
          rtn = true;
          CURL plex(url);
          std::string title = (*variantIt)["title"].asString();
          std::string key = (*variantIt)["key"].asString();
          std::string fastKey = (*variantIt)["fastKey"].asString();
          CFileItemPtr pItem(new CFileItem(title));
          pItem->m_bIsFolder = true;
          pItem->m_bIsShareOrDrive = false;
          if (fastKey.empty())
          {
            if (librarySectionID == "6")
            {
              removeLeadingSlash(key);
              plex.SetFileName(key);
            }
            else
              plex.SetFileName(plex.GetFileName() + "/" + key);
          }
          else
          {
            removeLeadingSlash(fastKey);
            std::vector<std::string> split = StringUtils::Split(fastKey, "?");
            std::string value = StringUtils::Split(split[1], "=")[0];
            plex.SetFileName(split[0]);
            plex.SetProtocolOption(value, key);
          }
          pItem->SetPath(parentPath + Base64URL::Encode(plex.Get()));
          pItem->SetLabel(title);
          pItem->SetProperty("SkipLocalArt", true);
          SetPlexItemProperties(*pItem);
          items.Add(pItem);
        }
      }
    }
    StringUtils::ToCapitalize(title2);
    items.SetLabel(title2);
  }
  
  return rtn;
}

bool CPlexUtils::GetItemSubtiles(CFileItem &item)
{
  std::string url = URIUtils::GetParentPath(item.GetPath());
  if (StringUtils::StartsWithNoCase(url, "plex://"))
    url = Base64URL::Decode(URIUtils::GetFileName(item.GetPath()));
  
  std::string id = item.GetMediaServiceId();
  std::string filename = StringUtils::Format("library/metadata/%s", id.c_str());
  
  CURL curl(url);
  curl.SetFileName(filename);

  CVariant variant = GetPlexCVariant(curl.Get());
  if (!variant.isNull() && variant.isObject() && variant.isMember("MediaContainer"))
  {
    const CVariant video(variant["MediaContainer"]["Video"]);
    if (!video.isNull())
    {
      const CVariant variantMedia = makeVariantArrayIfSingleItem(video["Media"]);
      for (auto variantIt = variantMedia.begin_array(); variantIt != variantMedia.end_array(); ++variantIt)
      {
        if (*variantIt != CVariant::VariantTypeNull)
          GetMediaDetals(item, curl, *variantIt, item.GetProperty("PlexMediaID").asString());
      }
    }
  }

  return true;
}

bool CPlexUtils::GetMoreItemInfo(CFileItem &item)
{
  std::string url = URIUtils::GetParentPath(item.GetPath());
  if (StringUtils::StartsWithNoCase(url, "plex://"))
    url = Base64URL::Decode(URIUtils::GetFileName(item.GetPath()));
  
  std::string childElement = "Video";
  std::string id = item.GetMediaServiceId();
  if (item.HasProperty("PlexShowKey") && item.GetVideoInfoTag()->m_type != MediaTypeMovie)
  {
    id = item.GetProperty("PlexShowKey").asString();
    childElement = "Directory";
  }
  
  CURL curl(url);
  curl.SetFileName("library/metadata/" + id);
  curl.SetProtocolOption("includeExtras","1");
  CVariant variant = GetPlexCVariant(curl.Get());
  if (!variant.isNull() && variant.isObject() && variant.isMember("MediaContainer"))
  {
    const CVariant video(variant["MediaContainer"][childElement]);
    if (!video.isNull())
      GetVideoDetails(item, video);
  }

  return true;
}

bool CPlexUtils::GetMoreResolutions(CFileItem &item)
{
  std::string id = item.GetMediaServiceId();
  std::string url = item.GetVideoInfoTag()->m_strFileNameAndPath;

  if (URIUtils::IsStack(url))
    url = XFILE::CStackDirectory::GetFirstStackedFile(url);
  else
    url = URIUtils::GetParentPath(url);

  CURL curl(url);
  curl.SetFileName("library/metadata/" + id);

  CContextButtons choices;
  std::vector<CFileItem> resolutionList;

  RemoveSubtitleProperties(item);
  CVariant variant = GetPlexCVariant(curl.Get());
  if (!variant.isNull() && variant.isObject() && variant.isMember("MediaContainer"))
  {
    const CVariant video(variant["MediaContainer"]["Video"]);
    if (!video.isNull())
    {
      const CVariant variantMedia = makeVariantArrayIfSingleItem(video["Media"]);
      for (auto variantIt = variantMedia.begin_array(); variantIt != variantMedia.end_array(); ++variantIt)
      {
        if (*variantIt != CVariant::VariantTypeNull)
        {
          CFileItem mediaItem(item);
          GetMediaDetals(mediaItem, curl, *variantIt);
          resolutionList.push_back(mediaItem);
          choices.Add(resolutionList.size(), mediaItem.GetProperty("PlexResolutionChoice").c_str());
        }
      }
    }
    if (resolutionList.size() < 2)
      return true;

    int button = CGUIDialogContextMenu::ShowAndGetChoice(choices);
    if (button > -1)
    {
      m_curItem = resolutionList[button - 1];
      item.UpdateInfo(m_curItem, false);
      item.SetPath(m_curItem.GetPath());
      return true;
    }
  }

  return false;
}

bool CPlexUtils::SearchPlex(CFileItemList &items, std::string strSearchString)
{
  if (CPlexServices::GetInstance().HasClients())
  {
    CFileItemList plexItems;
    std::vector<CPlexClientPtr> clients;
    CPlexServices::GetInstance().GetClients(clients);
    for (const auto &client : clients)
    {
      CURL url(client->GetUrl());
      url.SetFileName("hubs/search");
      url.SetProtocolOption("query",strSearchString);

      CVariant variant = GetPlexCVariant(url.Get());
      url.RemoveProtocolOption("query");
      if (!variant.isNull() && variant.isObject() && variant.isMember("MediaContainer"))
      {
        const CVariant variantHub = makeVariantArrayIfSingleItem(variant["MediaContainer"]["Hub"]);
        if (!variantHub.isNull())
        {
          for (auto variantIt = variantHub.begin_array(); variantIt != variantHub.end_array(); ++variantIt)
          {
            if (*variantIt == CVariant::VariantTypeNull)
              continue;
            const auto hub = *variantIt;

            if (hub.isMember("type"))
            {
              std::string type = hub["type"].asString();
              int size = hub["size"].asInteger();
              if (type == "show" && size > 0)
              {
                CFileItemList plexShow;
                std::string ratingKey = hub["Directory"]["ratingKey"].asString();
                url.SetFileName("library/metadata/"+ ratingKey + "/allLeaves");
                CVariant episodes = GetPlexCVariant(url.Get());
                if (!episodes.isNull() && episodes.isObject() && variant.isMember("MediaContainer"))
                  ParsePlexVideos(plexShow, url, episodes["MediaContainer"]["Video"], MediaTypeEpisode, false);
                
                for (int i = 0; i < plexShow.Size(); ++i)
                {
                  std::string label = plexShow[i]->GetVideoInfoTag()->m_strTitle + " (" +  plexShow[i]->GetVideoInfoTag()->m_strShowTitle + ")";
                  plexShow[i]->SetLabel(label);
                }
                CGUIWindowVideoBase::AppendAndClearSearchItems(plexShow, "[" + g_localizeStrings.Get(20359) + "] ", plexItems);
              }
              else if (type == "movie"  && size > 0)
              {
                CFileItemList plexMovies;
                ParsePlexVideos(plexMovies, url, hub["Video"], MediaTypeMovie, false);
                for (int i = 0; i < plexMovies.Size(); ++i)
                {
                  std::string label = plexMovies[i]->GetVideoInfoTag()->m_strTitle;
                  if (plexMovies[i]->GetVideoInfoTag()->GetYear() > 0)
                    label += StringUtils::Format(" (%i)", plexMovies[i]->GetVideoInfoTag()->GetYear());
                  plexMovies[i]->SetLabel(label);
                }
                CGUIWindowVideoBase::AppendAndClearSearchItems(plexMovies, "[" + g_localizeStrings.Get(20338) + "] ", plexItems);
              }
            }
          }
        }
      }

      for (int item = 0; item < plexItems.Size(); ++item)
        CPlexUtils::SetPlexItemProperties(*plexItems[item], client);
      
      SetPlexItemProperties(plexItems);
      items.Append(plexItems);
      plexItems.ClearItems();
    }
  }
  
  return items.Size() > 0;
}

#pragma mark - Plex Movies
bool CPlexUtils::GetPlexMovies(CFileItemList &items, std::string url, std::string filter)
{
  bool rtn = false;
  CURL curl(url);
  CVariant variant = GetPlexCVariant(url);
  if (!variant.isNull() && variant.isObject() && variant.isMember("MediaContainer"))
    rtn = ParsePlexVideos(items, curl, variant["MediaContainer"]["Video"], MediaTypeMovie, false);

  return rtn;
}

#pragma mark - Plex TV
bool CPlexUtils::GetPlexTvshows(CFileItemList &items, std::string url)
{
  bool rtn = false;
  CVariant variant = GetPlexCVariant(url);
  if (!variant.isNull() && variant.isObject() && variant.isMember("MediaContainer"))
  {
    CURL curl(url);
    std::string token = curl.GetProtocolOption("X-Plex-Token");
    curl.SetProtocolOptions("");
    curl.SetProtocolOption("X-Plex-Token",token);
    rtn = ParsePlexSeries(items, curl, variant["MediaContainer"]["Directory"]);
  }

  return rtn;
}

bool CPlexUtils::GetPlexSeasons(CFileItemList &items, const std::string url)
{
  bool rtn = false;
  CVariant variant = GetPlexCVariant(url);
  if (!variant.isNull() && variant.isObject() && variant.isMember("MediaContainer"))
  {
    CURL curl(url);
    rtn = ParsePlexSeasons(items, curl, variant["MediaContainer"], variant["MediaContainer"]["Directory"]);
  }

  return rtn;
}

bool CPlexUtils::GetPlexEpisodes(CFileItemList &items, const std::string url)
{
  bool rtn = false;
  CURL curl(url);
  CVariant variant = GetPlexCVariant(url);
  if (!variant.isNull() && variant.isObject() && variant.isMember("MediaContainer"))
  {
    int season = variant["MediaContainer"]["parentIndex"].asInteger();
    rtn = ParsePlexVideos(items, curl, variant["MediaContainer"]["Video"], MediaTypeEpisode, true, season);
    if (rtn)
      items.SetLabel(variant["MediaContainer"]["title2"].asString());
  }

  return rtn;
}
bool CPlexUtils::GetURL(CFileItem &item)
{
  if (!CSettings::GetInstance().GetBool(CSettings::SETTING_SERVICES_PLEXTRANSCODE))
    return true;

  CURL url(item.GetPath());
  std::string cleanUrl = url.GetWithoutFilename();
  CURL curl(cleanUrl);

  bool transcode = true;
  if ((!CSettings::GetInstance().GetBool(CSettings::SETTING_SERVICES_PLEXTRANSCODELOCAL)) &&
        CPlexServices::GetInstance().ClientIsLocal(cleanUrl))
  {
    transcode = false;
    /// transcode h265 or VC1 if user selected it
    if (CSettings::GetInstance().GetBool(CSettings::SETTING_SERVICES_PLEXTRANSCODELOCALEXCLUSION) &&
          item.GetVideoInfoTag()->m_streamDetails.GetVideoCodec() == "vc1")
        transcode = true;
    
    if (CSettings::GetInstance().GetBool(CSettings::SETTING_SERVICES_PLEXTRANSCODELOCALEXCLUSIONHEVC) &&
          item.GetVideoInfoTag()->m_streamDetails.GetVideoCodec() == "hevc")
      transcode = true;
  }

  if (!transcode)
    return true;
  
  int res = CSettings::GetInstance().GetInt(CSettings::SETTING_SERVICES_PLEXQUALITY);

  std::string maxBitrate;
  std::string resolution;

  switch (res)
  {
    case  1:
      maxBitrate = "320";
      resolution = "576x320";
      break;
    case  2:
      maxBitrate = "720";
      resolution = "720x480";
      break;
    case  3:
      maxBitrate = "1500";
      resolution = "1024x768";
      break;
    case  4:
      maxBitrate = "2000";
      resolution = "1280x720";
      break;
    case  5:
      maxBitrate = "3000";
      resolution = "1280x720";
      break;
    case  6:
      maxBitrate = "4000";
      resolution = "1920x1080";
      break;
    case  7:
      maxBitrate = "8000";
      resolution = "1920x1080";
      break;
    case  8:
      maxBitrate = "10000";
      resolution = "1920x1080";
      break;
    case  9:
      maxBitrate = "12000";
      resolution = "1920x1080";
      break;
    case  10:
      maxBitrate = "20000";
      resolution = "1920x1080";
      break;
    case  11:
      maxBitrate = "40000";
      resolution = "1920x1080";
      break;
    default:
      maxBitrate = "2000";
      resolution = "1280x720";
      break;
  }

  CLog::Log(LOGDEBUG, "CPlexUtils::GetURL - bitrate [%s] res [%s]", maxBitrate.c_str(), resolution.c_str());

  std::string plexID = item.GetMediaServiceId();
  std::string uuidStr = CSettings::GetInstance().GetString(CSettings::SETTING_SERVICES_UUID);

#if 1
  curl.SetFileName("video/:/transcode/universal/start.m3u8");
  curl.SetOption("hasMDE", "1");
  curl.SetOption("maxVideoBitrate", maxBitrate);
  curl.SetOption("protocol", "hls");
  curl.SetOption("secondsPerSegment", "10");
  curl.SetOption("session", uuidStr);
  /*
  std::string resumeTime = StringUtils::Format("%f", item.GetVideoInfoTag()->m_resumePoint.timeInSeconds);
  CLog::Log(LOGNOTICE, "resumeTime: %s seconds", resumeTime.c_str());
  curl.SetOption("offset", resumeTime);
  */
  curl.SetOption("offset", "0");
  curl.SetOption("videoQuality", "100");
  curl.SetOption("videoResolution", resolution);
  curl.SetOption("directStream", "1");
  curl.SetOption("directPlay", "0");
  curl.SetOption("audioBoost", "0");
  curl.SetOption("fastSeek", "1");
  curl.SetOption("subtitleSize", "100");
  curl.SetOption("location", CPlexServices::GetInstance().ClientIsLocal(cleanUrl) ? "lan" : "wan");
  curl.SetOption("path", "library/metadata/" + plexID);
#else
  curl.SetFileName("video/:/transcode/universal/start.mkv");
  curl.SetOption("hasMDE", "1");
  curl.SetOption("maxVideoBitrate", maxBitrate);
  curl.SetOption("session", uuidStr);
  std::string resumeTime = StringUtils::Format("%f", item.GetVideoInfoTag()->m_resumePoint.timeInSeconds);
  CLog::Log(LOGNOTICE, "resumeTime: %s seconds", resumeTime.c_str());
  curl.SetOption("offset", resumeTime);
  curl.SetOption("videoQuality", "100");
  curl.SetOption("videoResolution", resolution);
  curl.SetOption("directStream", "1");
  curl.SetOption("directPlay", "0");
  curl.SetOption("audioBoost", "0");
  curl.SetOption("fastSeek", "1");
  curl.SetOption("subtitleSize", "100");
  curl.SetOption("copyts", "1");
  curl.SetOption("partIndex", "0");
  curl.SetOption("mediaIndex", "0");
  int bufferSizeKBs = 20 * 1024;
  curl.SetOption("mediaBufferSize", StringUtils::Format("%d", bufferSizeKBs));
  // plex has duration in milliseconds
  std::string duration = StringUtils::Format("%i", item.GetVideoInfoTag()->m_duration * 1000);
  curl.SetOption("duration", duration);
  curl.SetOption("location", CPlexServices::GetInstance().ClientIsLocal(cleanUrl) ? "lan" : "wan");
  curl.SetOption("path", "library/metadata/" + plexID);
#endif

  // do we want audio transcoded?
  if (!CSettings::GetInstance().GetBool(CSettings::SETTING_SERVICES_PLEXTRANSCODEAUDIO))
  {
    curl.SetOption("X-Plex-Client-Profile-Extra", "append-transcode-target-audio-codec(type=videoProfile&context=streaming&codec=h264&protocol=hls&container=mpegts&audioCodec=dts,ac3,dca,mp3,eac3,truehd)");
    curl.SetOption("X-Plex-Client-Capabilities","protocols=http-live-streaming,http-mp4-streaming,http-mp4-video,http-mp4-video-720p,http-mp4-video-1080p,http-streaming-video,http-streaming-video-720p,http-streaming-video-1080p;videoDecoders=mpeg1video,mpeg2video,mpeg4,h264{profile:high&resolution:1080&level:51};audioDecoders=*");
  }
  //+add-limitation(scope=videoAudioCodec&scopeName=dca&type=upperBound&name=audio.channels&value=6&isRequired=false)
 // +add-direct-play-profile(type=videoProfile&container=mkv&codec=mpeg1video,mpeg2video,mpeg4,h264&audioCodec=pcm,mp3,ac3,dts,dca,eac3,mp2,flac,truehd,lpcm)
  else
  {
    curl.SetOption("X-Plex-Client-Capabilities","protocols=http-live-streaming,http-mp4-streaming,http-mp4-video,http-mp4-video-720p,http-mp4-video-1080p,http-streaming-video,http-streaming-video-720p,http-streaming-video-1080p;videoDecoders=mpeg4,h264{profile:high&resolution:1080&level:51};audioDecoders=aac");
    curl.SetOption("X-Plex-Client-Profile-Extra", "append-transcode-target-audio-codec(type=videoProfile&context=streaming&protocol=hls&container=mpegts&audioCodec=aac)");
  }
  curl.SetProtocolOption("X-Plex-Token", url.GetProtocolOption("X-Plex-Token"));
  curl.SetProtocolOption("X-Plex-Platform", "MrMC");
  curl.SetProtocolOption("X-Plex-Device","Plex Home Theater");
  curl.SetProtocolOption("X-Plex-Client-Identifier", uuidStr);

  item.SetPath(curl.Get());
  item.SetProperty("PlexTranscoder", true);
  return true;
}

void CPlexUtils::StopTranscode(CFileItem &item)
{
  CURL url(item.GetPath());
  std::string cleanUrl = url.Get();
  CURL curl(cleanUrl);

  std::string uuidStr = CSettings::GetInstance().GetString(CSettings::SETTING_SERVICES_UUID);

  CURL url1(item.GetPath());
  CURL url2(URIUtils::GetParentPath(cleanUrl));
  CURL url3(url2.GetWithoutFilename());
  url3.SetProtocolOption("X-Plex-Token",url1.GetProtocolOption("X-Plex-Token"));
  cleanUrl = url3.Get();

  if (StringUtils::StartsWithNoCase(cleanUrl, "plex://"))
    cleanUrl = Base64URL::Decode(URIUtils::GetFileName(item.GetPath()));

  std::string filename = StringUtils::Format("video/:/transcode/universal/stop?session=%s",uuidStr.c_str());
  ReportToServer(cleanUrl, filename);
}

void CPlexUtils::PingTranscoder(CFileItem &item)
{
  CURL url(item.GetPath());
  std::string cleanUrl = url.Get();
  CURL curl(cleanUrl);

  std::string uuidStr = CSettings::GetInstance().GetString(CSettings::SETTING_SERVICES_UUID);

  CURL url1(item.GetPath());
  CURL url2(URIUtils::GetParentPath(cleanUrl));
  CURL url3(url2.GetWithoutFilename());
  url3.SetProtocolOption("X-Plex-Token",url1.GetProtocolOption("X-Plex-Token"));
  cleanUrl = url3.Get();

  if (StringUtils::StartsWithNoCase(cleanUrl, "plex://"))
    cleanUrl = Base64URL::Decode(URIUtils::GetFileName(item.GetPath()));

  std::string filename = StringUtils::Format("video/:/transcode/universal/ping?session=%s",uuidStr.c_str());
  ReportToServer(cleanUrl, filename);
}

#pragma mark - Plex Music
bool CPlexUtils::ShowMusicInfo(CFileItem item)
{
  std::string type = item.GetMusicInfoTag()->m_type;
  if (type == MediaTypeSong)
  {
    CGUIDialogSongInfo *dialog = (CGUIDialogSongInfo *)g_windowManager.GetWindow(WINDOW_DIALOG_SONG_INFO);
    if (dialog)
    {
      dialog->SetSong(&item);
      dialog->Open();
    }
  }
  else if (type == MediaTypeAlbum)
  {
    CGUIDialogMusicInfo *pDlgAlbumInfo = (CGUIDialogMusicInfo*)g_windowManager.GetWindow(WINDOW_DIALOG_MUSIC_INFO);
    if (pDlgAlbumInfo)
    {
      pDlgAlbumInfo->SetAlbum(item);
      pDlgAlbumInfo->Open();
    }
  }
  else if (type == MediaTypeArtist)
  {
    CGUIDialogMusicInfo *pDlgArtistInfo = (CGUIDialogMusicInfo*)g_windowManager.GetWindow(WINDOW_DIALOG_MUSIC_INFO);
    if (pDlgArtistInfo)
    {
      pDlgArtistInfo->SetArtist(item);
      pDlgArtistInfo->Open();
    }
  }
  return true;
}

bool CPlexUtils::GetPlexSongs(CFileItemList &items, std::string url)
{
  bool rtn = false;
  CVariant variant = GetPlexCVariant(url);
  if (!variant.isNull() && variant.isObject() && variant.isMember("MediaContainer"))
  {
    CURL curl(url);
    rtn = ParsePlexSongs(items, curl, variant["MediaContainer"]["Track"]);
  }

  return rtn;
}

bool CPlexUtils::GetPlexAlbumSongs(CFileItem item, CFileItemList &items)
{
  std::string url = item.GetPath();
  if (StringUtils::StartsWithNoCase(url, "plex://"))
    url = Base64URL::Decode(URIUtils::GetFileName(item.GetPath()));

  CURL curl(url);
  curl.SetFileName("library/metadata/" + item.GetProperty("PlexAlbumKey").asString() + "/children");
  curl.RemoveProtocolOption("X-Plex-Container-Start");
  curl.RemoveProtocolOption("X-Plex-Container-Size");
  return GetPlexSongs(items, curl.Get());
}

bool CPlexUtils::GetPlexArtistsOrAlbum(CFileItemList &items, std::string url, bool album)
{
  bool rtn = false;
  CURL curl(url);
  if(album)
  {
    if (!curl.HasProtocolOption("type"))
      curl.SetProtocolOption("type", "9");
    if (curl.HasProtocolOption("genre"))
      curl.RemoveProtocolOption("genre");
  }
  CVariant variant = GetPlexCVariant(curl.Get());
  if (!variant.isNull() && variant.isObject() && variant.isMember("MediaContainer"))
  {
    rtn = ParsePlexArtistsAlbum(items, curl, variant["MediaContainer"]["Directory"], album);
  }

  return rtn;
}

bool CPlexUtils::GetAllPlexRecentlyAddedAlbums(CFileItemList &items, int limit)
{
  if (CPlexServices::GetInstance().HasClients())
  {
    CFileItemList plexItems;
    int limitTo = CSettings::GetInstance().GetInt(CSettings::SETTING_SERVICES_PLEXLIMITHOMETO);
    if (limitTo < 2)
      return false;
    //look through all plex clients and pull recently added for each library section
    std::vector<CPlexClientPtr> clients;
    CPlexServices::GetInstance().GetClients(clients);
    for (const auto &client : clients)
    {
      if (limitTo == 2 && !client->IsOwned())
        continue;
      CURL curl(client->GetUrl());
      curl.SetProtocol(client->GetProtocol());
      curl.SetFileName(curl.GetFileName() + "hubs/home/recentlyAdded");
      
      curl.SetProtocolOption("type","8");
      curl.SetProtocolOptions(curl.GetProtocolOptions() + StringUtils::Format("&X-Plex-Container-Start=0&X-Plex-Container-Size=%i", limit));
      
      GetPlexArtistsOrAlbum(plexItems, curl.Get(), true);
      for (int item = 0; item < plexItems.Size(); ++item)
        CPlexUtils::SetPlexItemProperties(*plexItems[item], client);
      
      SetPlexItemProperties(plexItems);
      items.Append(plexItems);
      items.SetLabel("Recently Added Albums");
      items.ClearSortState();
      items.Sort(SortByDateAdded, SortOrderDescending);
      plexItems.ClearItems();
    }
  }
  return true;
}

bool CPlexUtils::GetPlexRecentlyAddedAlbums(CFileItemList &items, const std::string url, int limit)
{
  if (CPlexServices::GetInstance().HasClients())
  {

    bool rtn = false;
//    CURL curl(url);
    CFileItemList plexItems;
    CPlexClientPtr client = CPlexServices::GetInstance().FindClient(url);
    std::vector<PlexSectionsContent> contents;
    contents = client->GetArtistContent();
    for (const auto &content : contents)
    {
      CURL curl(client->GetUrl());
      curl.SetProtocol(client->GetProtocol());
      curl.SetFileName(curl.GetFileName() + content.section + "/recentlyAdded");
      //        curl.SetFileName(curl.GetFileName() + "recentlyAdded");
      curl.SetProtocolOptions(curl.GetProtocolOptions() + StringUtils::Format("&X-Plex-Container-Start=0&X-Plex-Container-Size=%i", limit));
      
      rtn = GetPlexArtistsOrAlbum(plexItems, curl.Get(), true);
      
      for (int item = 0; item < plexItems.Size(); ++item)
        CPlexUtils::SetPlexItemProperties(*plexItems[item], client);
    }
    SetPlexItemProperties(plexItems);
    items.Append(plexItems);
    items.SetLabel("Recently Added Albums");
    items.Sort(SortByDateAdded, SortOrderDescending);
    plexItems.ClearItems();
    items.SetProperty("library.filter", "true");
    items.GetMusicInfoTag()->m_type = MediaTypeAlbum;
    return rtn;
  }
  return true;
}

#pragma mark - Plex parsers
bool CPlexUtils::ParsePlexVideos(CFileItemList &items, CURL url, const CVariant &videos, std::string type, bool formatLabel, int season /* = -1 */)
{
  bool rtn = false;
  // null videos is ok in some usage
  if (videos.isNull())
    return rtn;

  std::string value;
  std::string imagePath;
  url.RemoveProtocolOption("type");
  url.RemoveProtocolOption("X-Plex-Container-Start");
  url.RemoveProtocolOption("X-Plex-Container-Size");
  const CVariant variantVideo = makeVariantArrayIfSingleItem(videos);
  for (auto variantIt = variantVideo.begin_array(); variantIt != variantVideo.end_array(); ++variantIt)
  {
    if (*variantIt == CVariant::VariantTypeNull)
      continue;
    const auto item = *variantIt;

    rtn = true;
    CFileItemPtr plexItem(new CFileItem());

    std::string fanart;
    std::string value;
    // if we have season means we are listing episodes, we need to get the fanart from rootXmlNode.
    // movies has it in videoNode
    if (season > -1)
    {
      value = item["thumb"].asString();
      url.SetFileName("photo/:/transcode");
      url.SetOption("height", "800");
      url.SetOption("width", "800");
      url.SetOption("url", value);
      imagePath = url.Get();
      plexItem->SetArt("thumb", imagePath);
      plexItem->SetArt("tvshow.thumb", imagePath);
      plexItem->SetIconImage(imagePath);
      fanart = item["art"].asString();
      plexItem->GetVideoInfoTag()->m_iSeason = season;
      plexItem->GetVideoInfoTag()->m_iEpisode = item["index"].asInteger();
      plexItem->GetVideoInfoTag()->m_strShowTitle = item["grandparentTitle"].asString();
    }
    else if (!item["grandparentTitle"].isNull()) // only recently added episodes have this
    {
      fanart = item["art"].asString();

      plexItem->GetVideoInfoTag()->m_iSeason = item["parentIndex"].asInteger();
      plexItem->GetVideoInfoTag()->m_iEpisode = item["index"].asInteger();
      plexItem->GetVideoInfoTag()->m_strShowTitle = item["grandparentTitle"].asString();

      value = item["thumb"].asString();
      url.SetFileName("photo/:/transcode");
      url.SetOption("height", "800");
      url.SetOption("width", "800");
      url.SetOption("url", value);
      imagePath = url.Get();
      plexItem->SetArt("thumb", imagePath);

      value = item["parentThumb"].asString();
      url.SetOption("url", value);
      imagePath = url.Get();
      plexItem->SetArt("season.poster", imagePath);

      value = item["grandparentThumb"].asString();
      url.SetOption("url", value);
      imagePath = url.Get();
      plexItem->SetArt("tvshow.poster", imagePath);
      plexItem->SetArt("tvshow.thumb", imagePath);

      plexItem->SetIconImage(imagePath);

      std::string seasonEpisode = StringUtils::Format("S%02iE%02i", plexItem->GetVideoInfoTag()->m_iSeason, plexItem->GetVideoInfoTag()->m_iEpisode);
      plexItem->SetProperty("SeasonEpisode", seasonEpisode);
    }
    else
    {
      fanart = item["art"].asString();
      plexItem->SetLabel(item["title"].asString());

      value = item["thumb"].asString();
      url.SetFileName("photo/:/transcode");
      url.SetOption("height", "800");
      url.SetOption("width", "800");
      url.SetOption("url", value);
      imagePath = url.Get();
      plexItem->SetArt("thumb", imagePath);
      plexItem->SetIconImage(imagePath);
    }

    std::string title = item["title"].asString();
    plexItem->SetLabel(title);
    plexItem->GetVideoInfoTag()->m_strTitle = title;
    plexItem->SetMediaServiceId(item["ratingKey"].asString());
    plexItem->SetProperty("PlexShowKey", item["grandparentRatingKey"].asString());
    plexItem->GetVideoInfoTag()->m_type = type;
    plexItem->GetVideoInfoTag()->SetPlotOutline(item["tagline"].asString());
    plexItem->GetVideoInfoTag()->SetPlot(item["summary"].asString());

    CDateTime firstAired;
    firstAired.SetFromDBDate(item["originallyAvailableAt"].asString());
    plexItem->GetVideoInfoTag()->m_firstAired = firstAired;

    time_t addedTime = item["addedAt"].asInteger();
    plexItem->GetVideoInfoTag()->m_dateAdded = CDateTime(addedTime);
    
    time_t lastPlayed = item["lastViewedAt"].asInteger();
    plexItem->GetVideoInfoTag()->m_lastPlayed = CDateTime(lastPlayed);

    url.SetFileName("photo/:/transcode");
    url.SetOption("height", "1080");
    url.SetOption("width", "1920");
    url.SetOption("quality", "90");
    url.SetOption("url", fanart);
    plexItem->SetArt("fanart", url.Get());

    plexItem->GetVideoInfoTag()->SetYear(item["year"].asInteger());
    
    // Set ratings
    SetPlexRatingProperties(*plexItem, item);
    
    // lastViewedAt means that it was watched, if so we set m_playCount to 1 and set overlay.
    // If we have "viewOffset" that means we are partially watched and shoudl not set m_playCount to 1
    if (item.isMember("lastViewedAt") && !item.isMember("viewOffset"))
      plexItem->GetVideoInfoTag()->m_playCount = 1;

    plexItem->SetOverlayImage(CGUIListItem::ICON_OVERLAY_UNWATCHED, plexItem->HasVideoInfoTag() && plexItem->GetVideoInfoTag()->m_playCount > 0);

    GetVideoDetails(*plexItem, item);

    CBookmark m_bookmark;
    m_bookmark.timeInSeconds = item["viewOffset"].asInteger() / 1000;
    m_bookmark.totalTimeInSeconds = item["duration"].asInteger() / 1000;;
    plexItem->GetVideoInfoTag()->m_resumePoint = m_bookmark;
    plexItem->m_lStartOffset = item["viewOffset"].asInteger() / 1000;

    const CVariant media = makeVariantArrayIfSingleItem(item["Media"]);
    GetMediaDetals(*plexItem, url, media[0]);

    if (formatLabel)
    {
      CLabelFormatter formatter("%H. %T", "");
      formatter.FormatLabel(plexItem.get());
      plexItem->SetLabelPreformated(true);
    }
    SetPlexItemProperties(*plexItem);
    items.Add(plexItem);
  }
  // this is needed to display movies/episodes properly ... dont ask
  // good thing it didnt take 2 days to figure it out
  items.SetProperty("library.filter", "true");
  SetPlexItemProperties(items);

  return rtn;
}

bool CPlexUtils::ParsePlexSeries(CFileItemList &items, const CURL &url, const CVariant &directory)
{
  bool rtn = false;
  const CVariant variantDirectory = makeVariantArrayIfSingleItem(directory);
  if (variantDirectory.isNull() || !variantDirectory.isArray())
  {
    CLog::Log(LOGERROR, "CPlexUtils::ParsePlexSeries invalid response from %s", url.GetRedacted().c_str());
    return rtn;
  }

  CURL curl(url);
  curl.RemoveProtocolOption("X-Plex-Container-Start");
  curl.RemoveProtocolOption("X-Plex-Container-Size");
  if (curl.HasOption("genre"))
    curl.RemoveOption("genre");
  std::string value;
  for (auto variantIt = variantDirectory.begin_array(); variantIt != variantDirectory.end_array(); ++variantIt)
  {
    if (*variantIt == CVariant::VariantTypeNull)
      continue;
    const auto item = *variantIt;

    rtn = true;
    CFileItemPtr plexItem(new CFileItem());
    curl.SetOptions("");
    // set m_bIsFolder to true to indicate we are tvshow list
    plexItem->m_bIsFolder = true;
    plexItem->SetLabel(item["title"].asString());
    curl.SetFileName("library/metadata/" + item["ratingKey"].asString() + "/children");
    plexItem->SetPath("plex://tvshows/shows/" + Base64URL::Encode(curl.Get()));
    plexItem->SetMediaServiceId(item["ratingKey"].asString());
    plexItem->SetProperty("PlexShowKey", item["ratingKey"].asString());
    plexItem->GetVideoInfoTag()->m_type = MediaTypeTvShow;
    plexItem->GetVideoInfoTag()->m_strTitle = item["title"].asString();
    plexItem->GetVideoInfoTag()->SetPlotOutline(item["tagline"].asString());
    plexItem->GetVideoInfoTag()->SetPlot(item["summary"].asString());

    value = item["thumb"].asString();
    curl.SetFileName("photo/:/transcode");
    curl.SetOption("height", "800");
    curl.SetOption("width", "800");
    curl.SetOption("url", value);
    plexItem->SetArt("thumb", curl.Get());

    value = item["banner"].asString();
    curl.SetOption("height", "720");
    curl.SetOption("width", "1280");
    curl.SetOption("quality", "90");
    curl.SetOption("url", value);
    
    plexItem->SetArt("banner", curl.Get());

    value = item["art"].asString();
    curl.SetOption("height", "1080");
    curl.SetOption("width", "1920");
    curl.SetOption("quality", "90");
    curl.SetOption("url", value);
    plexItem->SetArt("fanart", curl.Get());

    plexItem->GetVideoInfoTag()->SetYear(item["year"].asInteger());
    plexItem->GetVideoInfoTag()->SetRating(item["rating"].asFloat());
    plexItem->GetVideoInfoTag()->m_strMPAARating = item["contentRating"].asString();

    time_t addedTime = item["addedAt"].asInteger();
    CDateTime aTime(addedTime);
    int watchedEpisodes = item["viewedLeafCount"].asInteger();
    int iSeasons        = item["childCount"].asInteger();
    plexItem->GetVideoInfoTag()->m_dateAdded = aTime;
    plexItem->GetVideoInfoTag()->m_iSeason = iSeasons;
    plexItem->GetVideoInfoTag()->m_iEpisode = item["leafCount"].asInteger();
    plexItem->GetVideoInfoTag()->m_playCount = (int)watchedEpisodes >= plexItem->GetVideoInfoTag()->m_iEpisode;

    plexItem->SetProperty("totalseasons", iSeasons);
    plexItem->SetProperty("totalepisodes", plexItem->GetVideoInfoTag()->m_iEpisode);
    plexItem->SetProperty("numepisodes", plexItem->GetVideoInfoTag()->m_iEpisode);
    plexItem->SetProperty("watchedepisodes", watchedEpisodes);
    plexItem->SetProperty("unwatchedepisodes", plexItem->GetVideoInfoTag()->m_iEpisode - watchedEpisodes);

    plexItem->SetOverlayImage(CGUIListItem::ICON_OVERLAY_UNWATCHED, watchedEpisodes >= plexItem->GetVideoInfoTag()->m_iEpisode);

    CDateTime firstAired;
    firstAired.SetFromDBDate(item["originallyAvailableAt"].asString());
    plexItem->GetVideoInfoTag()->m_firstAired = firstAired;

    GetVideoDetails(*plexItem, item);
    SetPlexItemProperties(*plexItem);
    items.Add(plexItem);
  }
  items.SetProperty("library.filter", "true");
  items.GetVideoInfoTag()->m_type = MediaTypeTvShow;

  return rtn;
}

bool CPlexUtils::ParsePlexSeasons(CFileItemList &items, const CURL &url, const CVariant &mediacontainer, const CVariant &directory)
{
  if (mediacontainer.isNull() || !mediacontainer.isObject() || directory.isNull())
  {
    CLog::Log(LOGERROR, "CPlexUtils::ParsePlexSeasons invalid response from %s", url.GetRedacted().c_str());
    return false;
  }

  bool rtn = false;
  std::string value;
  CURL curl(url);
  curl.RemoveProtocolOption("X-Plex-Container-Start");
  curl.RemoveProtocolOption("X-Plex-Container-Size");

  const CVariant variantDirectory = makeVariantArrayIfSingleItem(directory);
  for (auto variantIt = variantDirectory.begin_array(); variantIt != variantDirectory.end_array(); ++variantIt)
  {
    if (*variantIt == CVariant::VariantTypeNull)
      continue;
    const auto item = *variantIt;
    curl.SetOptions("");
    // only get the seasons listing, the one with "ratingKey"
    if (item.isMember("ratingKey"))
    {
      rtn = true;
      CFileItemPtr plexItem(new CFileItem());

      plexItem->m_bIsFolder = true;
      plexItem->SetLabel(item["title"].asString());
      curl.SetFileName("library/metadata/" + item["ratingKey"].asString() + "/children");
      plexItem->SetPath("plex://tvshows/seasons/" + Base64URL::Encode(curl.Get()));
      plexItem->SetMediaServiceId(item["ratingKey"].asString());
      plexItem->GetVideoInfoTag()->m_type = MediaTypeSeason;
      plexItem->GetVideoInfoTag()->m_strTitle = item["title"].asString();
      // we get these from rootXmlNode, where all show info is
      plexItem->GetVideoInfoTag()->m_strShowTitle = mediacontainer["parentTitle"].asString();
      plexItem->GetVideoInfoTag()->SetPlotOutline(mediacontainer["tagline"].asString());
      plexItem->GetVideoInfoTag()->SetPlot(mediacontainer["summary"].asString());
      plexItem->GetVideoInfoTag()->SetYear(mediacontainer["parentYear"].asInteger());
      plexItem->SetProperty("PlexShowKey", mediacontainer["key"].asString());

      value = mediacontainer["art"].asString();
      curl.SetFileName("photo/:/transcode");
      curl.SetOption("height", "800");
      curl.SetOption("width", "800");
      curl.SetOption("url", value);
      plexItem->SetArt("fanart", curl.Get());

      value = mediacontainer["banner"].asString();
      curl.SetOption("height", "720");
      curl.SetOption("width", "1280");
      curl.SetOption("quality", "90");
      curl.SetOption("url", value);
      plexItem->SetArt("banner", curl.Get());

      /// -------
      value = item["thumb"].asString();
      curl.SetFileName("photo/:/transcode");
      curl.SetOption("height", "800");
      curl.SetOption("width", "800");
      curl.SetOption("url", value);
      plexItem->SetArt("thumb", curl.Get());

      time_t addedTime = item["addedAt"].asInteger();
      plexItem->GetVideoInfoTag()->m_dateAdded = CDateTime(addedTime);

      int watchedEpisodes = item["viewedLeafCount"].asInteger();
      int iSeason = item["index"].asInteger();
      plexItem->GetVideoInfoTag()->m_iSeason = iSeason;
      plexItem->GetVideoInfoTag()->m_iEpisode = item["leafCount"].asInteger();
      plexItem->GetVideoInfoTag()->m_playCount = (int)watchedEpisodes >= plexItem->GetVideoInfoTag()->m_iEpisode;

      plexItem->SetProperty("totalepisodes", plexItem->GetVideoInfoTag()->m_iEpisode);
      plexItem->SetProperty("numepisodes", plexItem->GetVideoInfoTag()->m_iEpisode);
      plexItem->SetProperty("watchedepisodes", watchedEpisodes);
      plexItem->SetProperty("unwatchedepisodes", plexItem->GetVideoInfoTag()->m_iEpisode - watchedEpisodes);

      plexItem->SetOverlayImage(CGUIListItem::ICON_OVERLAY_UNWATCHED, watchedEpisodes >= plexItem->GetVideoInfoTag()->m_iEpisode);

      SetPlexItemProperties(*plexItem);
      items.Add(plexItem);
    }
  }
  items.SetLabel(mediacontainer["title2"].asString());

  if (!items.IsEmpty())
  {
    int iFlatten = CSettings::GetInstance().GetInt(CSettings::SETTING_VIDEOLIBRARY_FLATTENTVSHOWS);
    int itemsSize = items.GetObjectCount();
    int firstIndex = items.Size() - itemsSize;

    std::string showTitle;

    // check if the last item is the "All seasons" item which should be ignored for flattening
    if (!items[items.Size() - 1]->HasVideoInfoTag() || items[items.Size() - 1]->GetVideoInfoTag()->m_iSeason < 0)
      itemsSize -= 1;

    bool bFlatten = (itemsSize == 1 && iFlatten == 1) || iFlatten == 2 ||                              // flatten if one one season or if always flatten is enabled
    (itemsSize == 2 && iFlatten == 1 &&                                                // flatten if one season + specials
     (items[firstIndex]->GetVideoInfoTag()->m_iSeason == 0 || items[firstIndex + 1]->GetVideoInfoTag()->m_iSeason == 0));

    if (iFlatten > 0 && !bFlatten && (WatchedMode)CMediaSettings::GetInstance().GetWatchedMode("tvshows") == WatchedModeUnwatched)
    {
      int count = 0;
      for(int i = 0; i < items.Size(); i++)
      {
        const CFileItemPtr item = items.Get(i);
        if (item->GetProperty("unwatchedepisodes").asInteger() != 0 && item->GetVideoInfoTag()->m_iSeason > 0)
          count++;
      }
      bFlatten = (count < 2); // flatten if there is only 1 unwatched season (not counting specials)
    }

    if (bFlatten)
    { // flatten if one season or flatten always
      CFileItemList tempItems;

      for(int i = 0; i < items.Size(); i++)
      {
        const CFileItemPtr item = items.Get(i);
        showTitle = item->GetVideoInfoTag()->m_strShowTitle;
        CURL curl1(url);
        CFileItemList temp;
        curl1.SetFileName("library/metadata/" + item->GetMediaServiceId() + "/children");
        XFILE::CDirectory::GetDirectory("plex://tvshows/seasons/" + Base64URL::Encode(curl1.Get()), temp);
        tempItems.Assign(temp, true);
      }

      items.Clear();
      items.Assign(tempItems);
      items.SetLabel(showTitle);
    }
  }
  items.SetProperty("library.filter", "true");
  SetPlexItemProperties(items);

  return rtn;
}

bool CPlexUtils::ParsePlexSongs(CFileItemList &items, const CURL &url, const CVariant &track)
{
  bool rtn = false;
  const CVariant variantTrack = makeVariantArrayIfSingleItem(track);
  if (variantTrack.isNull() || !variantTrack.isArray())
  {
    CLog::Log(LOGERROR, "CPlexUtils::ParsePlexSongs invalid response from %s", url.GetRedacted().c_str());
    return rtn;
  }

  CURL curl(url);
  curl.RemoveProtocolOption("X-Plex-Container-Start");
  curl.RemoveProtocolOption("X-Plex-Container-Size");
  std::string value;
  for (auto variantTrackIt = variantTrack.begin_array(); variantTrackIt != variantTrack.end_array(); ++variantTrackIt)
  {
    if (*variantTrackIt == CVariant::VariantTypeNull)
      continue;
    const auto item = *variantTrackIt;

    rtn = true;
    curl.SetOptions("");
    CFileItemPtr plexItem(new CFileItem());
    plexItem->SetLabel(item["title"].asString());

    if (!item["Media"].isNull())
    {
      CVariant part = item["Media"]["Part"];
      if (!part.isNull())
      {
        std::string key = part["key"].asString();
        removeLeadingSlash(key);
        curl.SetFileName(key);
        plexItem->SetPath(curl.Get());

        plexItem->SetMediaServiceId(item["ratingKey"].asString());
        plexItem->SetProperty("PlexSongKey", item["ratingKey"].asString());
        plexItem->GetMusicInfoTag()->m_type = MediaTypeSong;
        plexItem->GetMusicInfoTag()->SetTitle(item["title"].asString());
        plexItem->SetLabel(item["title"].asString());
        plexItem->GetMusicInfoTag()->SetArtist(item["grandparentTitle"].asString());
        plexItem->GetMusicInfoTag()->SetAlbum(item["parentTitle"].asString());
        plexItem->GetMusicInfoTag()->SetYear(item["year"].asInteger());
        plexItem->GetMusicInfoTag()->SetTrackNumber(item["index"].asInteger());
        plexItem->GetMusicInfoTag()->SetDuration(item["duration"].asInteger()/1000);

        value = item["thumb"].asString();
        curl.SetFileName("photo/:/transcode");
        curl.SetOption("height", "800");
        curl.SetOption("width", "800");
        curl.SetOption("url", value);
        plexItem->SetArt("thumb", curl.Get());

        value = item["art"].asString();
        curl.SetOption("height", "1080");
        curl.SetOption("width", "1920");
        curl.SetOption("quality", "90");
        curl.SetOption("url", value);
        plexItem->SetArt("fanart", curl.Get());

        time_t addedTime = item["addedAt"].asInteger();
        plexItem->GetMusicInfoTag()->SetDateAdded(CDateTime(addedTime));
        plexItem->GetMusicInfoTag()->SetLoaded(true);
        SetPlexItemProperties(*plexItem);
        items.Add(plexItem);
      }
    }
  }
  items.SetProperty("library.filter", "true");
  items.GetMusicInfoTag()->m_type = MediaTypeSong;
  SetPlexItemProperties(items);

  return rtn;
}

bool CPlexUtils::ParsePlexArtistsAlbum(CFileItemList &items, const CURL &url, const CVariant &directory, bool album)
{
  bool rtn = false;
  if (directory.isNull())
  {
    CLog::Log(LOGERROR, "CPlexUtils::ParsePlexArtistsAlbum directory is null %s", url.GetRedacted().c_str());
    return rtn;
  }

  CURL curl(url);
  curl.RemoveProtocolOption("X-Plex-Container-Start");
  curl.RemoveProtocolOption("X-Plex-Container-Size");
  std::string value;
  std::string strMediaType = album ? MediaTypeAlbum : MediaTypeArtist;
  std::string strMediaTypeUrl = album ? "plex://music/songs/" : "plex://music/albums/";

  const CVariant variantDirectory = makeVariantArrayIfSingleItem(directory);
  for (auto variantIt = variantDirectory.begin_array(); variantIt != variantDirectory.end_array(); ++variantIt)
  {
    if (*variantIt == CVariant::VariantTypeNull)
      continue;
    const auto item = *variantIt;

    rtn = true;
    curl.SetOptions("");
    CFileItemPtr plexItem(new CFileItem());
    // set m_bIsFolder to true to indicate we are artist list
    plexItem->m_bIsFolder = true;
    plexItem->SetLabel(item["title"].asString());
    curl.SetFileName("library/metadata/" + item["ratingKey"].asString() + "/children");
    plexItem->SetPath(strMediaTypeUrl + Base64URL::Encode(curl.Get()));
    plexItem->SetMediaServiceId(item["ratingKey"].asString());

    plexItem->GetMusicInfoTag()->m_type = strMediaType;
    plexItem->GetMusicInfoTag()->SetTitle(item["title"].asString());
    if (album)
    {
      if (item.isMember("summary") && item["summary"].size() > 1)
        plexItem->SetProperty("album_description", item["summary"].asString());
      plexItem->GetMusicInfoTag()->SetArtistDesc(item["parentTitle"].asString());
      plexItem->SetProperty("artist", item["parentTitle"].asString());
      plexItem->SetProperty("PlexAlbumKey", item["ratingKey"].asString());
    }
    else
    {
      plexItem->GetMusicInfoTag()->SetArtistDesc(item["title"].asString());
      plexItem->SetProperty("PlexArtistKey", item["ratingKey"].asString());
    }
    plexItem->GetMusicInfoTag()->SetAlbum(item["title"].asString());
    plexItem->GetMusicInfoTag()->SetYear(item["year"].asInteger());

    value = item["thumb"].asString();
    curl.SetFileName("photo/:/transcode");
    curl.SetOption("height", "800");
    curl.SetOption("width", "800");
    curl.SetOption("url", value);
    plexItem->SetArt("thumb", curl.Get());
    plexItem->SetProperty("thumb", curl.Get());

    value = item["art"].asString();
    curl.SetOption("height", "1080");
    curl.SetOption("width", "1920");
    curl.SetOption("quality", "90");
    curl.SetOption("url", value);
    plexItem->SetArt("fanart", curl.Get());
    plexItem->SetProperty("fanart", curl.Get());

    time_t addedTime = item["addedAt"].asInteger();
    plexItem->GetMusicInfoTag()->SetDateAdded(CDateTime(addedTime));

    GetMusicDetails(*plexItem, item);
    SetPlexItemProperties(*plexItem);
    items.Add(plexItem);
  }
  items.SetProperty("library.filter", "true");
  items.GetVideoInfoTag()->m_type = strMediaType;
  SetPlexItemProperties(items);

  return rtn;
}

#pragma mark - Plex private
void CPlexUtils::ReportToServer(std::string url, std::string filename)
{
  CURL url2(url);
  url2.SetFileName(filename.c_str());

  std::string strXML;
  XFILE::CCurlFile plex;
  CPlexUtils::GetDefaultHeaders(&plex);
  plex.Get(url2.Get(), strXML);
}

void CPlexUtils::GetVideoDetails(CFileItem &item, const CVariant &video)
{
  if (item.GetVideoInfoTag()->m_strPlotOutline.empty())
    item.GetVideoInfoTag()->SetPlotOutline(video["tagline"].asString());
  if (item.GetVideoInfoTag()->m_strPlot.empty())
    item.GetVideoInfoTag()->SetPlot(video["summary"].asString());

  // looks like plex is sending only one studio?
  std::vector<std::string> studios;
  studios.push_back(video["studio"].asString());
  item.GetVideoInfoTag()->m_studio = studios;

  // get all genres
  std::vector<std::string> genres;
  const CVariant variantGenre = makeVariantArrayIfSingleItem(video["Genre"]);
  if (!variantGenre.isNull())
  {
    for (auto variantIt = variantGenre.begin_array(); variantIt != variantGenre.end_array(); ++variantIt)
    {
      if (*variantIt != CVariant::VariantTypeNull)
        genres.push_back((*variantIt)["tag"].asString());
    }
  }
  item.GetVideoInfoTag()->SetGenre(genres);

  // get all writers
  std::vector<std::string> writers;
  const CVariant variantWriter = makeVariantArrayIfSingleItem(video["Writer"]);
  if (!variantWriter.isNull())
  {
    for (auto variantIt = variantWriter.begin_array(); variantIt != variantWriter.end_array(); ++variantIt)
    {
      if (*variantIt != CVariant::VariantTypeNull)
        writers.push_back((*variantIt)["tag"].asString());
    }
  }
  item.GetVideoInfoTag()->SetWritingCredits(writers);

  // get all directors
  std::vector<std::string> directors;
  const CVariant variantDirector = makeVariantArrayIfSingleItem(video["Director"]);
  if (!variantDirector.isNull())
  {
    for (auto variantIt = variantDirector.begin_array(); variantIt != variantDirector.end_array(); ++variantIt)
    {
      CVariant dir = *variantIt;
      if (dir != CVariant::VariantTypeNull)
        directors.push_back(dir["tag"].asString());
    }
  }
  item.GetVideoInfoTag()->SetDirector(directors);

  // get all countries
  std::vector<std::string> countries;
  const CVariant variantCountry = makeVariantArrayIfSingleItem(video["Country"]);
  if (!variantCountry.isNull())
  {
    for (auto variantIt = variantCountry.begin_array(); variantIt != variantCountry.end_array(); ++variantIt)
    {
      if (*variantIt != CVariant::VariantTypeNull)
        countries.push_back((*variantIt)["tag"].asString());
    }
  }
  item.GetVideoInfoTag()->SetCountry(countries);

  // get all roles
  std::vector< SActorInfo > roles;
  const CVariant variantRoles = makeVariantArrayIfSingleItem(video["Role"]);
  if (!variantRoles.isNull())
  {
    for (auto variantIt = variantRoles.begin_array(); variantIt != variantRoles.end_array(); ++variantIt)
    {
      if (*variantIt != CVariant::VariantTypeNull)
      {
        SActorInfo role;
        std::string strRole;
        std::string strThumb = (*variantIt)["thumb"].asString();
        if ((*variantIt)["role"].asString() != "0")
          strRole =(*variantIt)["role"].asString();
        role.strName = (*variantIt)["tag"].asString();
        role.strRole = strRole;
        role.thumb   = strThumb;
        role.strMonogram = StringUtils::Monogram(role.strName);
        roles.push_back(role);
      }
    }
  }
  item.GetVideoInfoTag()->m_cast = roles;
  
  // get trailers
  std::vector<std::string> extras;
  const CVariant variantExtras = makeVariantArrayIfSingleItem(video["Extras"]["Video"]);
  if (!variantExtras.isNull())
  {
    for (auto variantItE = variantExtras.begin_array(); variantItE != variantExtras.end_array(); ++variantItE)
    {
      const auto vItem = *variantItE;
      // extraType == 1 is Trailer, thats the only one we want
      if (vItem["extraType"].asInteger() == 1)
      {
        if (!vItem["Media"].isNull())
        {
          const CVariant variantMedia = makeVariantArrayIfSingleItem(vItem["Media"]);
          for (auto variantPartIt = variantMedia.begin_array(); variantPartIt != variantMedia.end_array(); ++variantPartIt)
          {
            if (*variantPartIt == CVariant::VariantTypeNull)
              continue;
            const auto part = *variantPartIt;
            if (!part.isNull())
            {
              std::string key = part["Part"]["key"].asString();
              CURL trailerUrl(item.GetPath());
              removeLeadingSlash(key);
              trailerUrl.SetFileName(key);
              item.GetVideoInfoTag()->m_strTrailer = trailerUrl.Get();
              break;
            }
          }
        }
      }
    }
  }
}

void CPlexUtils::GetMusicDetails(CFileItem &item, const CVariant &video)
{
  // get all genres
  std::vector<std::string> genres;
  const CVariant variantGenre = video["Genre"];
  if (!variantGenre.isNull())
  {
    for (auto variantIt = variantGenre.begin_array(); variantIt != variantGenre.end_array(); ++variantIt)
    {
      if (*variantIt != CVariant::VariantTypeNull)
        genres.push_back((*variantIt)["tag"].asString());
    }
  }
  item.GetMusicInfoTag()->SetGenre(genres);

/*
  // get all countries
  std::vector<std::string> countries;
  const CVariant variantCountry = video["Country"];
  if (!variantCountry.isNull())
  {
    for (auto variantIt = variantDirector.begin_array(); variantIt != variantDirector.end_array(); ++variantIt)
    {
      if (*variantIt != CVariant::VariantTypeNull)
        countries.push_back((*variantIt)["tag"].asString());
    }
  }
  GetMusicInfoTag()->SetCountry(countries);
*/
}

void CPlexUtils::GetMediaDetals(CFileItem &item, CURL url, const CVariant &media, std::string id)
{
  url.SetOptions("");
  if (!media.isNull() && (id == "0" || media["id"].asString() == id))
  {
    CStreamDetails details;
    CStreamDetailVideo *p = new CStreamDetailVideo();
    p->m_strCodec = media["videoCodec"].asString();
    p->m_fAspect = media["aspectRatio"].asFloat();
    p->m_iWidth = media["width"].asInteger();
    p->m_iHeight = media["height"].asInteger();
    p->m_iDuration = media["duration"].asInteger() / 1000;
    details.AddStream(p);

    CStreamDetailAudio *a = new CStreamDetailAudio();
    a->m_strCodec = media["audioCodec"].asString();
    a->m_iChannels = media["audioChannels"].asInteger();
    a->m_strLanguage = media["audioChannels"].asString();
    details.AddStream(a);

    std::string label;
    float bitrate = media["bitrate"].asFloat() / 1000;
    std::string resolution = media["videoResolution"].asString();
    if (resolution.empty())
      label = StringUtils::Format("%.2f Mbps", bitrate);
    else
    {
      StringUtils::ToUpper(resolution);
      if (bitrate > 0)
        label = StringUtils::Format("%s, %.2f Mbps", resolution.c_str(), bitrate);
      else
        label = resolution;
    }
    item.SetProperty("PlexResolutionChoice", label);
    item.SetProperty("PlexMediaID", media["id"].asInteger());
    item.GetVideoInfoTag()->m_streamDetails = details;
    /// plex has duration in milliseconds
    item.GetVideoInfoTag()->m_duration = media["duration"].asInteger() / 1000;

    int iPart = 1;
    std::string filePath;
    const CVariant variantPart = makeVariantArrayIfSingleItem(media["Part"]);
    for (auto variantPartIt = variantPart.begin_array(); variantPartIt != variantPart.end_array(); ++variantPartIt)
    {
      if (*variantPartIt == CVariant::VariantTypeNull)
        continue;
      const auto part = *variantPartIt;

      int iSubtile = 1;
      std::string subFile;
      const CVariant variantStream = makeVariantArrayIfSingleItem(part["Stream"]);
      for (auto variantIt = variantStream.begin_array(); variantIt != variantStream.end_array(); ++variantIt)
      {
        if (*variantIt == CVariant::VariantTypeNull)
          continue;
        const auto stream = *variantIt;

        // "key" indicates that subtitle file is external, we only need external as our player will pick up internal
        bool externalSubtitle = stream.isMember("key");
        if (externalSubtitle && stream["streamType"].asInteger() == 3)
        {
          CURL plex(url);
          std::string id    = stream["id"].asString();
          std::string ext   = stream["format"].asString();
          std::string codec = stream["codec"].asString();
          std::string filename = stream["key"].asString();
          removeLeadingSlash(filename);
          plex.SetFileName(filename);
          std::string propertyKey = StringUtils::Format("subtitle:%i", iSubtile);
          item.SetProperty(propertyKey, plex.Get());
          std::string propertyLangKey = StringUtils::Format("subtitle:%i_language", iSubtile);
          std::string languageCode = stream["languageCode"].asString();
          item.SetProperty(propertyLangKey, languageCode);
          std::string propertyForcedKey = StringUtils::Format("subtitle:%i_forced", iSubtile);
          item.SetProperty(propertyForcedKey, stream["forced"].asBoolean());
          iSubtile++;
        }
      }

      if (iPart == 2)
        filePath = "stack://" + filePath;
      std::string key = part["key"].asString();
      removeLeadingSlash(key);
      url.SetFileName(key);
      item.SetMediaServiceFile(part["file"].asString());
      std::string propertyKey = StringUtils::Format("stack:%i_time", iPart);
      item.SetProperty(propertyKey, part["duration"].asInteger() / 1000);

      if (iPart > 1)
      {
        filePath = filePath + " , " + url.Get();
        CLog::Log(LOGDEBUG, "CPlexUtils::GetMediaDetals iPart > 1 %s", item.GetLabel().c_str());
      }
      else
        filePath = url.Get();
      iPart++;
    }
    item.SetPath(filePath);
    item.GetVideoInfoTag()->m_strFileNameAndPath = filePath;
  }
}

CVariant CPlexUtils::GetPlexCVariant(std::string url, std::string filter)
{
#if defined(PLEX_DEBUG_TIMING)
  unsigned int currentTime = XbmcThreads::SystemClockMillis();
#endif
  XFILE::CCurlFile curlfile;
  curlfile.SetRequestHeader("Accept-Encoding", "gzip");
  GetDefaultHeaders(&curlfile);

  StringUtils::Replace(url, "|","?");
  CURL curl(url);
  // this is key to get back gzip encoded content
  curl.SetProtocolOption("seekable", "0");
  if (!filter.empty())
    curl.SetFileName(curl.GetFileName() + filter);

  std::string response;
  if (curlfile.Get(curl.Get(), response))
  {

#if defined(PLEX_DEBUG_TIMING)
    CLog::Log(LOGDEBUG, "CPlexUtils::GetPlexCVariant %d(msec) for %lu bytes",
              XbmcThreads::SystemClockMillis() - currentTime, response.size());
#endif

    if (curlfile.GetContentEncoding() == "gzip")
    {
      std::string buffer;
      if (XFILE::CZipFile::DecompressGzip(response, buffer))
        response = std::move(buffer);
      else
        return CVariant(CVariant::VariantTypeNull);
    }

#if defined(PLEX_DEBUG_VERBOSE)
    CLog::Log(LOGDEBUG, "CPlexUtils::GetPlexCVariant %s", curl.Get().c_str());
    CLog::Log(LOGDEBUG, "CPlexUtils::GetPlexCVariant %s", response.c_str());
#endif
#if defined(PLEX_DEBUG_TIMING)
    currentTime = XbmcThreads::SystemClockMillis();
#endif
    // be careful, xml2json will mess with response somehow
    // so make a copy of you want to use it later.
    std::string jsonBody = xml2json(response.c_str());
    CVariant resultObject;
    if (CJSONVariantParser::Parse(jsonBody, resultObject))
    {
  #if defined(PLEX_DEBUG_TIMING)
      CLog::Log(LOGDEBUG, "CPlexUtils::GetPlexCVariant parsed in %d(msec)",
                  XbmcThreads::SystemClockMillis() - currentTime);
  #endif
      // recently added does not return proper object, we make one up later
      if (resultObject.isObject() || resultObject.isArray())
        return resultObject;
    }
  }

  return CVariant(CVariant::VariantTypeNull);
}

TiXmlDocument CPlexUtils::GetPlexXML(std::string url, std::string filter)
{
#if defined(PLEX_DEBUG_TIMING)
  unsigned int currentTime = XbmcThreads::SystemClockMillis();
#endif
  XFILE::CCurlFile curlfile;
  curlfile.SetRequestHeader("Accept-Encoding", "gzip");
  GetDefaultHeaders(&curlfile);

  CURL curl(url);
  // this is key to get back gzip encoded content
  curl.SetProtocolOption("seekable", "0");
  if (!filter.empty())
    curl.SetFileName(curl.GetFileName() + filter);

  std::string response;
  if (curlfile.Get(curl.Get(), response))
  {

#if defined(PLEX_DEBUG_TIMING)
    CLog::Log(LOGDEBUG, "CPlexUtils::GetPlexXML %d(msec) for %lu bytes",
              XbmcThreads::SystemClockMillis() - currentTime, response.size());
#endif
    if (curlfile.GetContentEncoding() == "gzip")
    {
      std::string buffer;
      if (XFILE::CZipFile::DecompressGzip(response, buffer))
        response = std::move(buffer);
      else
      {
        TiXmlDocument xml;
        return xml;
      }
    }

#if defined(PLEX_DEBUG_VERBOSE)
    CLog::Log(LOGDEBUG, "CPlexUtils::GetPlexXML %s", curl.Get().c_str());
    CLog::Log(LOGDEBUG, "CPlexUtils::GetPlexXML %s", response.c_str());
#endif
#if defined(PLEX_DEBUG_TIMING)
    currentTime = XbmcThreads::SystemClockMillis();
#endif

    TiXmlDocument xml;
    xml.Parse(response.c_str());
#if defined(PLEX_DEBUG_TIMING)
    CLog::Log(LOGDEBUG, "CPlexUtils::GetPlexXML parsed in %d(msec)",
                XbmcThreads::SystemClockMillis() - currentTime);
#endif
    return xml;
  }

  TiXmlDocument xml;
  return xml;
}

int CPlexUtils::ParsePlexCVariant(const CVariant &item)
{
  int result = item["totalSize"].asInteger();
  return result;
}

int CPlexUtils::ParsePlexMediaXML(TiXmlDocument xml)
{
  int result = 0;
  TiXmlElement* rootXmlNode = xml.RootElement();
  if (rootXmlNode)
  {
    result = atoi(XMLUtils::GetAttribute(rootXmlNode, "totalSize").c_str());
  }
  
  return result;
}

void CPlexUtils::RemoveSubtitleProperties(CFileItem &item)
{
  std::string key("subtitle:1");
  for(unsigned s = 1; item.HasProperty(key); key = StringUtils::Format("subtitle:%u", ++s))
  {
    item.ClearProperty(key);
    item.ClearProperty(StringUtils::Format("subtitle:%i_language", s));
  }
}
