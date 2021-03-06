/*
 *      Copyright (C) 2017 Christian Browet
 *      http://xbmc.org
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
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "JNIXBMCMediaSession.h"

#include <androidjni/jutils-details.hpp>
#include <androidjni/Context.h>

#include "CompileInfo.h"
#include "XBMCApp.h"
#include "Application.h"
#include "messaging/ApplicationMessenger.h"
#include "input/Key.h"

using namespace jni;

static std::string s_className = std::string(CCompileInfo::GetClass()) + "/XBMCMediaSession";

CJNIXBMCMediaSession::CJNIXBMCMediaSession()
  : CJNIBase(s_className)
  , m_isActive(false)
{
  m_object = new_object(CJNIContext::getClassLoader().loadClass(GetDotClassName(s_className)));
  m_object.setGlobal();

  add_instance(m_object, this);
}

CJNIXBMCMediaSession::CJNIXBMCMediaSession(const CJNIXBMCMediaSession& other)
  : CJNIBase(other)
{
  add_instance(m_object, this);
}

CJNIXBMCMediaSession::~CJNIXBMCMediaSession()
{
  remove_instance(this);
}

void CJNIXBMCMediaSession::RegisterNatives(JNIEnv* env)
{
  jclass cClass = env->FindClass(s_className.c_str());
  if(cClass)
  {
    JNINativeMethod methods[] =
    {
      {"_onPlayRequested", "()V", (void*)&CJNIXBMCMediaSession::_onPlayRequested},
      {"_onPauseRequested", "()V", (void*)&CJNIXBMCMediaSession::_onPauseRequested},
      {"_onNextRequested", "()V", (void*)&CJNIXBMCMediaSession::_onNextRequested},
      {"_onPreviousRequested", "()V", (void*)&CJNIXBMCMediaSession::_onPreviousRequested},
      {"_onForwardRequested", "()V", (void*)&CJNIXBMCMediaSession::_onForwardRequested},
      {"_onRewindRequested", "()V", (void*)&CJNIXBMCMediaSession::_onRewindRequested},
      {"_onStopRequested", "()V", (void*)&CJNIXBMCMediaSession::_onStopRequested},
      {"_onSeekRequested", "(J)V", (void*)&CJNIXBMCMediaSession::_onSeekRequested},
    };

    env->RegisterNatives(cClass, methods, sizeof(methods)/sizeof(methods[0]));
  }
}

void CJNIXBMCMediaSession::activate(bool state)
{
  if (state == m_isActive)
    return;

  call_method<void>(m_object,
                    "activate", "(Z)V",
                    (jboolean)state);
  m_isActive = state;
}

void CJNIXBMCMediaSession::updatePlaybackState(const CJNIPlaybackState& state)
{
  call_method<void>(m_object,
                    "updatePlaybackState", "(Landroid/media/session/PlaybackState;)V",
                    state.get_raw());
}

void CJNIXBMCMediaSession::updateMetadata(const CJNIMediaMetadata& myData)
{
  call_method<void>(m_object,
                    "updateMetadata", "(Landroid/media/MediaMetadata;)V",
                    myData.get_raw());
}

void CJNIXBMCMediaSession::updateIntent(const CJNIIntent& intent)
{
  call_method<void>(m_object,
                    "updateIntent", "(Landroid/content/Intent;)V",
                    intent.get_raw());
}

void CJNIXBMCMediaSession::OnPlayRequested()
{
  if (m_isActive && g_application.m_pPlayer->IsPlaying())
  {
    KODI::MESSAGING::CApplicationMessenger::GetInstance().PostMsg(TMSG_GUI_ACTION, WINDOW_INVALID, -1, static_cast<void*>(new CAction(ACTION_PLAYER_PLAYPAUSE)));
  }
}

void CJNIXBMCMediaSession::OnPauseRequested()
{
  if (m_isActive && g_application.m_pPlayer->IsPlaying())
  {
    KODI::MESSAGING::CApplicationMessenger::GetInstance().PostMsg(TMSG_GUI_ACTION, WINDOW_INVALID, -1, static_cast<void*>(new CAction(ACTION_PLAYER_PLAYPAUSE)));
  }
}

void CJNIXBMCMediaSession::OnNextRequested()
{
  if (m_isActive && g_application.m_pPlayer->IsPlaying())
    KODI::MESSAGING::CApplicationMessenger::GetInstance().PostMsg(TMSG_GUI_ACTION, WINDOW_INVALID, -1, static_cast<void*>(new CAction(ACTION_NEXT_ITEM)));
}

void CJNIXBMCMediaSession::OnPreviousRequested()
{
  if (m_isActive && g_application.m_pPlayer->IsPlaying())
    KODI::MESSAGING::CApplicationMessenger::GetInstance().PostMsg(TMSG_GUI_ACTION, WINDOW_INVALID, -1, static_cast<void*>(new CAction(ACTION_PREV_ITEM)));
}

void CJNIXBMCMediaSession::OnForwardRequested()
{
  if (m_isActive && g_application.m_pPlayer->IsPlaying())
  {
    if (!g_application.m_pPlayer->IsPaused())
      KODI::MESSAGING::CApplicationMessenger::GetInstance().PostMsg(TMSG_GUI_ACTION, WINDOW_INVALID, -1, static_cast<void*>(new CAction(ACTION_PLAYER_FORWARD)));
  }
}

void CJNIXBMCMediaSession::OnRewindRequested()
{
  if (m_isActive && g_application.m_pPlayer->IsPlaying())
  {
    if (!g_application.m_pPlayer->IsPaused())
      KODI::MESSAGING::CApplicationMessenger::GetInstance().PostMsg(TMSG_GUI_ACTION, WINDOW_INVALID, -1, static_cast<void*>(new CAction(ACTION_PLAYER_REWIND)));
  }
}

void CJNIXBMCMediaSession::OnStopRequested()
{
  if (m_isActive && g_application.m_pPlayer->IsPlaying())
    KODI::MESSAGING::CApplicationMessenger::GetInstance().PostMsg(TMSG_GUI_ACTION, WINDOW_INVALID, -1, static_cast<void*>(new CAction(ACTION_STOP)));
}

void CJNIXBMCMediaSession::OnSeekRequested(int64_t pos)
{
  if (m_isActive)
    g_application.SeekTime(pos / 1000.0);
}

bool CJNIXBMCMediaSession::isActive() const
{
  return m_isActive;
}

/**********************/

void CJNIXBMCMediaSession::_onPlayRequested(JNIEnv* env, jobject thiz)
{
  (void)env;

  CJNIXBMCMediaSession *inst = find_instance(thiz);
  if (inst)
    inst->OnPlayRequested();
}

void CJNIXBMCMediaSession::_onPauseRequested(JNIEnv* env, jobject thiz)
{
  (void)env;

  CJNIXBMCMediaSession *inst = find_instance(thiz);
  if (inst)
    inst->OnPauseRequested();
}

void CJNIXBMCMediaSession::_onNextRequested(JNIEnv* env, jobject thiz)
{
  (void)env;

  CJNIXBMCMediaSession *inst = find_instance(thiz);
  if (inst)
    inst->OnNextRequested();
}

void CJNIXBMCMediaSession::_onPreviousRequested(JNIEnv* env, jobject thiz)
{
  (void)env;

  CJNIXBMCMediaSession *inst = find_instance(thiz);
  if (inst)
    inst->OnPreviousRequested();
}

void CJNIXBMCMediaSession::_onForwardRequested(JNIEnv* env, jobject thiz)
{
  (void)env;

  CJNIXBMCMediaSession *inst = find_instance(thiz);
  if (inst)
    inst->OnForwardRequested();
}

void CJNIXBMCMediaSession::_onRewindRequested(JNIEnv* env, jobject thiz)
{
  (void)env;

  CJNIXBMCMediaSession *inst = find_instance(thiz);
  if (inst)
    inst->OnRewindRequested();
}

void CJNIXBMCMediaSession::_onStopRequested(JNIEnv* env, jobject thiz)
{
  (void)env;

  CJNIXBMCMediaSession *inst = find_instance(thiz);
  if (inst)
    inst->OnStopRequested();
}

void CJNIXBMCMediaSession::_onSeekRequested(JNIEnv* env, jobject thiz, jlong pos)
{
  (void)env;

  CJNIXBMCMediaSession *inst = find_instance(thiz);
  if (inst)
    inst->OnSeekRequested(pos);
}
