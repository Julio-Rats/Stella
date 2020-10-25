//============================================================================
//
//   SSSS    tt          lll  lll
//  SS  SS   tt           ll   ll
//  SS     tttttt  eeee   ll   ll   aaaa
//   SSSS    tt   ee  ee  ll   ll      aa
//      SS   tt   eeeeee  ll   ll   aaaaa  --  "An Atari 2600 VCS Emulator"
//  SS  SS   tt   ee      ll   ll  aa  aa
//   SSSS     ttt  eeeee llll llll  aaaaa
//
// Copyright (c) 1995-2020 by Bradford W. Mott, Stephen Anthony
// and the Stella Team
//
// See the file "License.txt" for information on usage and redistribution of
// this file, and for a DISCLAIMER OF ALL WARRANTIES.
//============================================================================

#include "bspf.hxx"
#include "Logger.hxx"

#include "Console.hxx"
#include "EventHandler.hxx"
#include "Event.hxx"
#include "OSystem.hxx"
#include "Settings.hxx"
#include "TIA.hxx"
#include "Sound.hxx"
#include "MediaFactory.hxx"

#include "FBSurface.hxx"
#include "TIASurface.hxx"
#include "FrameBuffer.hxx"
#include "StateManager.hxx"
#include "RewindManager.hxx"

#ifdef DEBUGGER_SUPPORT
  #include "Debugger.hxx"
#endif
#ifdef GUI_SUPPORT
  #include "Font.hxx"
  #include "StellaFont.hxx"
  #include "ConsoleMediumFont.hxx"
  #include "ConsoleMediumBFont.hxx"
  #include "StellaMediumFont.hxx"
  #include "StellaLargeFont.hxx"
  #include "Stella12x24tFont.hxx"
  #include "Stella14x28tFont.hxx"
  #include "Stella16x32tFont.hxx"
  #include "ConsoleFont.hxx"
  #include "ConsoleBFont.hxx"
  #include "Launcher.hxx"
  #include "Menu.hxx"
  #include "CommandMenu.hxx"
  #include "MessageMenu.hxx"
  #include "TimeMachine.hxx"
#endif

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
FrameBuffer::FrameBuffer(OSystem& osystem)
  : myOSystem(osystem)
{
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
FrameBuffer::~FrameBuffer()
{
  // Make sure to free surfaces/textures before destroying the backend itself
  // Most platforms are fine with doing this in either order, but it seems
  // that OpenBSD in particular crashes when attempting to destroy textures
  // *after* the renderer is already destroyed
  freeSurfaces();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void FrameBuffer::initialize()
{
  // First create the platform-specific backend; it is needed before anything
  // else can be used
  try { myBackend = MediaFactory::createVideoBackend(myOSystem); }
  catch(const runtime_error& e) { throw e; }

  // Get desktop resolution and supported renderers
  vector<Common::Size> windowedDisplays;
  myBackend->queryHardware(myFullscreenDisplays, windowedDisplays, myRenderers);
  uInt32 query_w = windowedDisplays[0].w, query_h = windowedDisplays[0].h;

  // Check the 'maxres' setting, which is an undocumented developer feature
  // that specifies the desktop size (not normally set)
  const Common::Size& s = myOSystem.settings().getSize("maxres");
  if(s.valid())
  {
    query_w = s.w;
    query_h = s.h;
  }
  // Various parts of the codebase assume a minimum screen size
  myAbsDesktopSize.w = std::max(query_w, FBMinimum::Width);
  myAbsDesktopSize.h = std::max(query_h, FBMinimum::Height);
  myDesktopSize = myAbsDesktopSize;

  // Check for HiDPI mode (is it activated, and can we use it?)
  myHiDPIAllowed = ((myAbsDesktopSize.w / 2) >= FBMinimum::Width) &&
    ((myAbsDesktopSize.h / 2) >= FBMinimum::Height);
  myHiDPIEnabled = myHiDPIAllowed && myOSystem.settings().getBool("hidpi");

  // In HiDPI mode, the desktop resolution is essentially halved
  // Later, the output is scaled and rendered in 2x mode
  if(hidpiEnabled())
  {
    myDesktopSize.w = myAbsDesktopSize.w / hidpiScaleFactor();
    myDesktopSize.h = myAbsDesktopSize.h / hidpiScaleFactor();
  }

#ifdef GUI_SUPPORT
  setupFonts();
#endif

  // Determine possible TIA windowed zoom levels
  myTIAMaxZoom = maxWindowZoom(TIAConstants::viewableWidth,
                               TIAConstants::viewableHeight);
  float currentTIAZoom = myOSystem.settings().getFloat("tia.zoom");
  myOSystem.settings().setValue("tia.zoom",
      BSPF::clampw(currentTIAZoom, supportedTIAMinZoom(), myTIAMaxZoom));

  setUIPalette();

  myGrabMouse = myOSystem.settings().getBool("grabmouse");

  // Create a TIA surface; we need it for rendering TIA images
  myTIASurface = make_unique<TIASurface>(myOSystem);
}

#ifdef GUI_SUPPORT
// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void FrameBuffer::setupFonts()
{
  ////////////////////////////////////////////////////////////////////
  // Create fonts to draw text
  // NOTE: the logic determining appropriate font sizes is done here,
  //       so that the UI classes can just use the font they expect,
  //       and not worry about it
  //       This logic should also take into account the size of the
  //       framebuffer, and try to be intelligent about font sizes
  //       We can probably add ifdefs to take care of corner cases,
  //       but that means we've failed to abstract it enough ...
  ////////////////////////////////////////////////////////////////////

  // This font is used in a variety of situations when a really small
  // font is needed; we let the specific widget/dialog decide when to
  // use it
  mySmallFont = make_unique<GUI::Font>(GUI::stellaDesc); // 6x10

  if(myOSystem.settings().getBool("minimal_ui"))
  {
    // The general font used in all UI elements
    myFont = make_unique<GUI::Font>(GUI::stella12x24tDesc);           // 12x24
    // The info font used in all UI elements
    myInfoFont = make_unique<GUI::Font>(GUI::stellaLargeDesc);        // 10x20
  }
  else
  {
    const int NUM_FONTS = 7;
    FontDesc FONT_DESC[NUM_FONTS] = {GUI::consoleDesc, GUI::consoleMediumDesc, GUI::stellaMediumDesc,
      GUI::stellaLargeDesc, GUI::stella12x24tDesc, GUI::stella14x28tDesc, GUI::stella16x32tDesc};
    const string& dialogFont = myOSystem.settings().getString("dialogfont");
    FontDesc fd = getFontDesc(dialogFont);

    // The general font used in all UI elements
    myFont = make_unique<GUI::Font>(fd);                                //  default: 9x18
    // The info font used in all UI elements,
    //  automatically determined aiming for 1 / 1.4 (~= 18 / 13) size
    int fontIdx = 0;
    for(int i = 0; i < NUM_FONTS; ++i)
    {
      if(fd.height <= FONT_DESC[i].height * 1.4)
      {
        fontIdx = i;
        break;
      }
    }
    myInfoFont = make_unique<GUI::Font>(FONT_DESC[fontIdx]);            //  default 8x13

    // Determine minimal zoom level based on the default font
    //  So what fits with default font should fit for any font.
    //  However, we have to make sure all Dialogs are sized using the fontsize.
    int zoom_h = (fd.height * 4 * 2) / GUI::stellaMediumDesc.height;
    int zoom_w = (fd.maxwidth * 4 * 2) / GUI::stellaMediumDesc.maxwidth;
    // round to 25% steps, >= 200%
    myTIAMinZoom = std::max(std::max(zoom_w, zoom_h) / 4.F, 2.F);
  }

  // The font used by the ROM launcher
  const string& lf = myOSystem.settings().getString("launcherfont");

  myLauncherFont = make_unique<GUI::Font>(getFontDesc(lf));       //  8x13
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
FontDesc FrameBuffer::getFontDesc(const string& name) const
{
  if(name == "small")
    return GUI::consoleBDesc;       //  8x13
  else if(name == "low_medium")
    return GUI::consoleMediumBDesc; //  9x15
  else if(name == "medium")
    return GUI::stellaMediumDesc;   //  9x18
  else if(name == "large" || name == "large10")
    return GUI::stellaLargeDesc;    // 10x20
  else if(name == "large12")
    return GUI::stella12x24tDesc;   // 12x24
  else if(name == "large14")
    return GUI::stella14x28tDesc;   // 14x28
  else // "large16"
    return GUI::stella16x32tDesc;   // 16x32
}
#endif

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
FBInitStatus FrameBuffer::createDisplay(const string& title, BufferType type,
                                        Common::Size size, bool honourHiDPI)
{
  ++myInitializedCount;
  myBackend->setTitle(title);

  // In HiDPI mode, all created displays must be scaled appropriately
  if(honourHiDPI && hidpiEnabled())
  {
    size.w *= hidpiScaleFactor();
    size.h *= hidpiScaleFactor();
  }

  // A 'windowed' system is defined as one where the window size can be
  // larger than the screen size, as there's some sort of window manager
  // that takes care of it (all current desktop systems fall in this category)
  // However, some systems have no concept of windowing, and have hard limits
  // on how large a window can be (ie, the size of the 'desktop' is the
  // absolute upper limit on window size)
  //
  // If the WINDOWED_SUPPORT macro is defined, we treat the system as the
  // former type; if not, as the latter type

#ifdef WINDOWED_SUPPORT
  // We assume that a desktop of at least minimum acceptable size means that
  // we're running on a 'large' system, and the window size requirements
  // can be relaxed
  // Otherwise, we treat the system as if WINDOWED_SUPPORT is not defined
  if(myDesktopSize.w < FBMinimum::Width && myDesktopSize.h < FBMinimum::Height &&
     size > myDesktopSize)
    return FBInitStatus::FailTooLarge;
#else
  // Make sure this mode is even possible
  // We only really need to worry about it in non-windowed environments,
  // where requesting a window that's too large will probably cause a crash
  if(size > myDesktopSize)
    return FBInitStatus::FailTooLarge;
#endif

  // Initialize video mode handler, so it can know what video modes are
  // appropriate for the requested image size
  myVidModeHandler.setImageSize(size);

  // Always save, maybe only the mode of the window has changed
  saveCurrentWindowPosition();
  myBufferType = type;

  // Initialize video subsystem
  string pre_about = myBackend->about();
  FBInitStatus status = applyVideoMode();
  if(status != FBInitStatus::Success)
    return status;

#ifdef GUI_SUPPORT
  // Erase any messages from a previous run
  myMsg.counter = 0;

  // Create surfaces for TIA statistics and general messages
  const GUI::Font& f = hidpiEnabled() ? infoFont() : font();
  myStatsMsg.color = kColorInfo;
  myStatsMsg.w = f.getMaxCharWidth() * 40 + 3;
  myStatsMsg.h = (f.getFontHeight() + 2) * 3;

  if(!myStatsMsg.surface)
  {
    myStatsMsg.surface = allocateSurface(myStatsMsg.w, myStatsMsg.h);
    myStatsMsg.surface->attributes().blending = true;
    myStatsMsg.surface->attributes().blendalpha = 92; //aligned with TimeMachineDialog
    myStatsMsg.surface->applyAttributes();
  }

  if(!myMsg.surface)
  {
    const int fontWidth = font().getMaxCharWidth(),
              HBORDER = fontWidth * 1.25 / 2.0;
    myMsg.surface = allocateSurface(fontWidth * MESSAGE_WIDTH + HBORDER * 2,
                                    font().getFontHeight() * 1.5);
  }
#endif

  // Print initial usage message, but only print it later if the status has changed
  if(myInitializedCount == 1)
  {
    Logger::info(myBackend->about());
  }
  else
  {
    string post_about = myBackend->about();
    if(post_about != pre_about)
      Logger::info(post_about);
  }

  return status;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void FrameBuffer::update(bool force)
{
  // Onscreen messages are a special case and require different handling than
  // other objects; they aren't UI dialogs in the normal sense nor are they
  // TIA images, and they need to be rendered on top of everything
  // The logic is split in two pieces:
  //  - at the top of ::update(), to determine whether underlying dialogs
  //    need to be force-redrawn
  //  - at the bottom of ::update(), to actually draw them (this must come
  //    last, since they are always drawn on top of everything else).

  // Full rendering is required when messages are enabled
  force = force || myMsg.counter >= 0;

  // Detect when a message has been turned off; one last redraw is required
  // in this case, to draw over the area that the message occupied
  if(myMsg.counter == 0)
    myMsg.counter = -1;

  switch(myOSystem.eventHandler().state())
  {
    case EventHandlerState::NONE:
    case EventHandlerState::EMULATION:
      // Do nothing; emulation mode is handled separately (see below)
      return;

    case EventHandlerState::PAUSE:
    {
      // Show a pause message immediately and then every 7 seconds
      if(myPausedCount-- <= 0)
      {
        myPausedCount = uInt32(7 * myOSystem.frameRate());
        showMessage("Paused", MessagePosition::MiddleCenter);
      }
      if(force)
        myTIASurface->render();

      break;  // EventHandlerState::PAUSE
    }

  #ifdef GUI_SUPPORT
    case EventHandlerState::OPTIONSMENU:
    {
      force = force || myOSystem.menu().needsRedraw();
      if(force)
      {
        clear();
        myTIASurface->render();
        myOSystem.menu().draw(force);
      }
      break;  // EventHandlerState::OPTIONSMENU
    }

    case EventHandlerState::CMDMENU:
    {
      force = force || myOSystem.commandMenu().needsRedraw();
      if(force)
      {
        clear();
        myTIASurface->render();
        myOSystem.commandMenu().draw(force);
      }
      break;  // EventHandlerState::CMDMENU
    }

    case EventHandlerState::MESSAGEMENU:
    {
      force = force || myOSystem.messageMenu().needsRedraw();
      if (force)
      {
        clear();
        myTIASurface->render();
        myOSystem.messageMenu().draw(force);
      }
      break;  // EventHandlerState::MESSAGEMENU
    }

    case EventHandlerState::TIMEMACHINE:
    {
      force = force || myOSystem.timeMachine().needsRedraw();
      if(force)
      {
        clear();
        myTIASurface->render();
        myOSystem.timeMachine().draw(force);
      }
      break;  // EventHandlerState::TIMEMACHINE
    }

    case EventHandlerState::PLAYBACK:
    {
      static Int32 frames = 0;
      RewindManager& r = myOSystem.state().rewindManager();
      bool success = true;
      Int64 frameCycles = 76 * std::max<Int32>(myOSystem.console().tia().scanlinesLastFrame(), 240);

      if(--frames <= 0)
      {
        r.unwindStates(1);
        // get time between current and next state
        uInt64 startCycles = r.getCurrentCycles();
        success = r.unwindStates(1);
        // display larger state gaps faster
        frames = std::sqrt((myOSystem.console().tia().cycles() - startCycles) / frameCycles);

        if(success)
          r.rewindStates(1);
      }

      force = force || success;
      if (force)
        myTIASurface->render();

      // Stop playback mode at the end of the state buffer
      // and switch to Time Machine or Pause mode
      if (!success)
      {
        frames = 0;
        myOSystem.eventHandler().enterMenuMode(EventHandlerState::TIMEMACHINE);
      }
      break;  // EventHandlerState::PLAYBACK
    }

    case EventHandlerState::LAUNCHER:
    {
      force = force || myOSystem.launcher().needsRedraw();
      if(force)
      {
        clear();
        myOSystem.launcher().draw(force);
      }
      break;  // EventHandlerState::LAUNCHER
    }
  #endif

  #ifdef DEBUGGER_SUPPORT
    case EventHandlerState::DEBUGGER:
    {
      force = force || myOSystem.debugger().needsRedraw();
      if(force)
      {
        clear();
        myOSystem.debugger().draw(force);
      }
      break;  // EventHandlerState::DEBUGGER
    }
  #endif
    default:
      break;
  }

  // Draw any pending messages
  // The logic here determines whether to draw the message
  // If the message is to be disabled, logic inside the draw method
  // indicates that, and then the code at the top of this method sees
  // the change and redraws everything
  if(myMsg.enabled)
    drawMessage();

  // Push buffers to screen only when necessary
  if(force)
    myBackend->renderToScreen();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void FrameBuffer::updateInEmulationMode(float framesPerSecond)
{
  // Update method that is specifically tailored to emulation mode
  //
  // We don't worry about selective rendering here; the rendering
  // always happens at the full framerate

  clear();  // TODO - test this: it may cause slowdowns on older systems
  myTIASurface->render();

  // Show frame statistics
  if(myStatsMsg.enabled)
    drawFrameStats(framesPerSecond);

  myLastScanlines = myOSystem.console().tia().frameBufferScanlinesLastFrame();
  myPausedCount = 0;

  // Draw any pending messages
  if(myMsg.enabled)
    drawMessage();

  // Push buffers to screen
  myBackend->renderToScreen();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void FrameBuffer::showMessage(const string& message, MessagePosition position,
                              bool force)
{
#ifdef GUI_SUPPORT
  // Only show messages if they've been enabled
  if(myMsg.surface == nullptr || !(force || myOSystem.settings().getBool("uimessages")))
    return;

  const int fontWidth  = font().getMaxCharWidth(),
            fontHeight = font().getFontHeight();
  const int VBORDER = fontHeight / 4;
  const int HBORDER = fontWidth * 1.25 / 2.0;

  myMsg.counter = uInt32(myOSystem.frameRate()) * 2; // Show message for 2 seconds
  if(myMsg.counter == 0)
    myMsg.counter = 120;

  // Precompute the message coordinates
  myMsg.text      = message;
  myMsg.color     = kBtnTextColor;
  myMsg.showGauge = false;
  myMsg.w         = std::min(fontWidth * (MESSAGE_WIDTH) - HBORDER * 2,
                             font().getStringWidth(myMsg.text) + HBORDER * 2);
  myMsg.h         = fontHeight + VBORDER * 2;
  myMsg.position  = position;
  myMsg.enabled   = true;

  myMsg.surface->setSrcSize(myMsg.w, myMsg.h);
  myMsg.surface->setDstSize(myMsg.w * hidpiScaleFactor(), myMsg.h * hidpiScaleFactor());
#endif
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void FrameBuffer::showMessage(const string& message, const string& valueText,
                              float value, float minValue, float maxValue)
{
#ifdef GUI_SUPPORT
  // Only show messages if they've been enabled
  if(myMsg.surface == nullptr || !myOSystem.settings().getBool("uimessages"))
    return;

  const int fontWidth  = font().getMaxCharWidth(),
            fontHeight = font().getFontHeight();
  const int VBORDER = fontHeight / 4;
  const int HBORDER = fontWidth * 1.25 / 2.0;

  myMsg.counter = uInt32(myOSystem.frameRate()) * 2; // Show message for 2 seconds
  if(myMsg.counter == 0)
    myMsg.counter = 120;

  // Precompute the message coordinates
  myMsg.text       = message;
  myMsg.color      = kBtnTextColor;
  myMsg.showGauge  = true;
  if(maxValue - minValue != 0)
    myMsg.value = (value - minValue) / (maxValue - minValue) * 100.F;
  else
    myMsg.value = 100.F;
  myMsg.valueText  = valueText;
  myMsg.w          = std::min(fontWidth * MESSAGE_WIDTH,
                              font().getStringWidth(myMsg.text)
                              + fontWidth * (GAUGEBAR_WIDTH + 2)
                              + font().getStringWidth(myMsg.valueText))
    + HBORDER * 2;
  myMsg.h          = fontHeight + VBORDER * 2;
  myMsg.position   = MessagePosition::BottomCenter;
  myMsg.enabled    = true;

  myMsg.surface->setSrcSize(myMsg.w, myMsg.h);
  myMsg.surface->setDstSize(myMsg.w * hidpiScaleFactor(), myMsg.h * hidpiScaleFactor());
#endif
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool FrameBuffer::messageShown() const
{
#ifdef GUI_SUPPORT
  return myMsg.enabled;
#else
  return false;
#endif
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void FrameBuffer::drawFrameStats(float framesPerSecond)
{
#ifdef GUI_SUPPORT
  const ConsoleInfo& info = myOSystem.console().about();
  int xPos = 2, yPos = 0;
  const GUI::Font& f = hidpiEnabled() ? infoFont() : font();
  const int dy = f.getFontHeight() + 2;

  ostringstream ss;

  myStatsMsg.surface->invalidate();

  // draw scanlines
  ColorId color = myOSystem.console().tia().frameBufferScanlinesLastFrame() !=
    myLastScanlines ? kDbgColorRed : myStatsMsg.color;

  ss
    << myOSystem.console().tia().frameBufferScanlinesLastFrame()
    << " / "
    << std::fixed << std::setprecision(1)
    << myOSystem.console().currentFrameRate()
    << "Hz => "
    << info.DisplayFormat;

  myStatsMsg.surface->drawString(f, ss.str(), xPos, yPos,
                                 myStatsMsg.w, color, TextAlign::Left, 0, true, kBGColor);

  yPos += dy;
  ss.str("");

  ss
    << std::fixed << std::setprecision(1) << framesPerSecond
    << "fps @ "
    << std::fixed << std::setprecision(0) << 100 *
      (myOSystem.settings().getBool("turbo")
        ? 20.0F
        : myOSystem.settings().getFloat("speed"))
    << "% speed";

  myStatsMsg.surface->drawString(f, ss.str(), xPos, yPos,
      myStatsMsg.w, myStatsMsg.color, TextAlign::Left, 0, true, kBGColor);

  yPos += dy;
  ss.str("");

  ss << info.BankSwitch;
  if (myOSystem.settings().getBool("dev.settings")) ss << "| Developer";

  myStatsMsg.surface->drawString(f, ss.str(), xPos, yPos,
      myStatsMsg.w, myStatsMsg.color, TextAlign::Left, 0, true, kBGColor);

  myStatsMsg.surface->setDstPos(imageRect().x() + 10, imageRect().y() + 8);
  myStatsMsg.surface->setDstSize(myStatsMsg.w * hidpiScaleFactor(),
                                 myStatsMsg.h * hidpiScaleFactor());
  myStatsMsg.surface->render();
#endif
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void FrameBuffer::toggleFrameStats(bool toggle)
{
  if (toggle)
    showFrameStats(!myStatsEnabled);
  myOSystem.settings().setValue(
    myOSystem.settings().getBool("dev.settings") ? "dev.stats" : "plr.stats", myStatsEnabled);

  myOSystem.frameBuffer().showMessage(string("Console info ") +
                                      (myStatsEnabled ? "enabled" : "disabled"));
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void FrameBuffer::showFrameStats(bool enable)
{
  myStatsEnabled = myStatsMsg.enabled = enable;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void FrameBuffer::enableMessages(bool enable)
{
  if(enable)
  {
    // Only re-enable frame stats if they were already enabled before
    myStatsMsg.enabled = myStatsEnabled;
  }
  else
  {
    // Temporarily disable frame stats
    myStatsMsg.enabled = false;

    // Erase old messages on the screen
    myMsg.enabled = false;
    myMsg.counter = 0;
    update(true);  // Force update immediately
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
inline bool FrameBuffer::drawMessage()
{
#ifdef GUI_SUPPORT
  // Either erase the entire message (when time is reached),
  // or show again this frame
  if(myMsg.counter == 0)
  {
    myMsg.enabled = false;
    return true;
  }
  else if(myMsg.counter < 0)
  {
    myMsg.enabled = false;
    return false;
  }

  // Draw the bounded box and text
  const Common::Rect& dst = myMsg.surface->dstRect();
  const int fontWidth  = font().getMaxCharWidth(),
            fontHeight = font().getFontHeight();
  const int VBORDER = fontHeight / 4;
  const int HBORDER = fontWidth * 1.25 / 2.0;
  constexpr int BORDER = 1;

  switch(myMsg.position)
  {
    case MessagePosition::TopLeft:
      myMsg.x = 5;
      myMsg.y = 5;
      break;

    case MessagePosition::TopCenter:
      myMsg.x = (imageRect().w() - dst.w()) >> 1;
      myMsg.y = 5;
      break;

    case MessagePosition::TopRight:
      myMsg.x = imageRect().w() - dst.w() - 5;
      myMsg.y = 5;
      break;

    case MessagePosition::MiddleLeft:
      myMsg.x = 5;
      myMsg.y = (imageRect().h() - dst.h()) >> 1;
      break;

    case MessagePosition::MiddleCenter:
      myMsg.x = (imageRect().w() - dst.w()) >> 1;
      myMsg.y = (imageRect().h() - dst.h()) >> 1;
      break;

    case MessagePosition::MiddleRight:
      myMsg.x = imageRect().w() - dst.w() - 5;
      myMsg.y = (imageRect().h() - dst.h()) >> 1;
      break;

    case MessagePosition::BottomLeft:
      myMsg.x = 5;
      myMsg.y = imageRect().h() - dst.h() - 5;
      break;

    case MessagePosition::BottomCenter:
      myMsg.x = (imageRect().w() - dst.w()) >> 1;
      myMsg.y = imageRect().h() - dst.h() - 5;
      break;

    case MessagePosition::BottomRight:
      myMsg.x = imageRect().w() - dst.w() - 5;
      myMsg.y = imageRect().h() - dst.h() - 5;
      break;
  }

  myMsg.surface->setDstPos(myMsg.x + imageRect().x(), myMsg.y + imageRect().y());
  myMsg.surface->fillRect(0, 0, myMsg.w, myMsg.h, kColor);
  myMsg.surface->fillRect(BORDER, BORDER, myMsg.w - BORDER * 2, myMsg.h - BORDER * 2, kBtnColor);
  myMsg.surface->drawString(font(), myMsg.text, HBORDER, VBORDER,
                            myMsg.w, myMsg.color);

  if(myMsg.showGauge)
  {
    constexpr int NUM_TICKMARKS = 4;
    // limit gauge bar width if texts are too long
    const int swidth = std::min(fontWidth * GAUGEBAR_WIDTH,
                                fontWidth * (MESSAGE_WIDTH - 2)
                                - font().getStringWidth(myMsg.text)
                                - font().getStringWidth(myMsg.valueText));
    const int bwidth = swidth * myMsg.value / 100.F;
    const int bheight = fontHeight >> 1;
    const int x = HBORDER + font().getStringWidth(myMsg.text) + fontWidth;
    // align bar with bottom of text
    const int y = VBORDER + font().desc().ascent - bheight;

    // draw gauge bar
    myMsg.surface->fillRect(x - BORDER, y, swidth + BORDER * 2, bheight, kSliderBGColor);
    myMsg.surface->fillRect(x, y + BORDER, bwidth, bheight - BORDER * 2, kSliderColor);
    // draw tickmark in the middle of the bar
    for(int i = 1; i < NUM_TICKMARKS; ++i)
    {
      ColorId color;
      int xt = x + swidth * i / NUM_TICKMARKS;
      if(bwidth < xt - x)
        color = kCheckColor; // kSliderColor;
      else
        color = kSliderBGColor;
      myMsg.surface->vLine(xt, y + bheight / 2, y + bheight - 1, color);
    }
    // draw value text
    myMsg.surface->drawString(font(), myMsg.valueText,
                              x + swidth + fontWidth, VBORDER,
                              myMsg.w, myMsg.color);
  }
  myMsg.surface->render();
  myMsg.counter--;
#endif

  return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void FrameBuffer::setPauseDelay()
{
  myPausedCount = uInt32(2 * myOSystem.frameRate());
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
shared_ptr<FBSurface> FrameBuffer::allocateSurface(
    int w, int h, ScalingInterpolation inter, const uInt32* data
)
{
  // Add new surface to the list
  mySurfaceList.push_back(myBackend->createSurface(w, h, inter, data));

  // And return a pointer to it (pointer should be treated read-only)
  return mySurfaceList.at(mySurfaceList.size() - 1);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void FrameBuffer::freeSurfaces()
{
  for(auto& s: mySurfaceList)
    s->free();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void FrameBuffer::reloadSurfaces()
{
  for(auto& s: mySurfaceList)
    s->reload();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void FrameBuffer::resetSurfaces()
{
  // Free all resources for each surface, then reload them
  // Due to possible timing and/or synchronization issues, all free()'s
  // are done first, then all reload()'s
  // Any derived FrameBuffer classes that call this method should be
  // aware of these restrictions, and act accordingly

  freeSurfaces();
  reloadSurfaces();

  update(true); // force full update
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void FrameBuffer::setTIAPalette(const PaletteArray& rgb_palette)
{
  // Create a TIA palette from the raw RGB data
  PaletteArray tia_palette;
  for(int i = 0; i < 256; ++i)
  {
    uInt8 r = (rgb_palette[i] >> 16) & 0xff;
    uInt8 g = (rgb_palette[i] >> 8) & 0xff;
    uInt8 b = rgb_palette[i] & 0xff;

    tia_palette[i] = mapRGB(r, g, b);
  }

  // Remember the TIA palette; place it at the beginning of the full palette
  std::copy_n(tia_palette.begin(), tia_palette.size(), myFullPalette.begin());

  // Let the TIA surface know about the new palette
  myTIASurface->setPalette(tia_palette, rgb_palette);

  // Since the UI palette shares the TIA palette, we need to update it too
  setUIPalette();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void FrameBuffer::setUIPalette()
{
  // Set palette for UI (upper area of full palette)
  const UIPaletteArray& ui_palette =
     (myOSystem.settings().getString("uipalette") == "classic") ? ourClassicUIPalette :
     (myOSystem.settings().getString("uipalette") == "light")   ? ourLightUIPalette :
     (myOSystem.settings().getString("uipalette") == "dark")    ? ourDarkUIPalette :
      ourStandardUIPalette;

  for(size_t i = 0, j = myFullPalette.size() - ui_palette.size();
      i < ui_palette.size(); ++i, ++j)
  {
    const uInt8 r = (ui_palette[i] >> 16) & 0xff,
                g = (ui_palette[i] >> 8) & 0xff,
                b =  ui_palette[i] & 0xff;

    myFullPalette[j] = mapRGB(r, g, b);
  }
  FBSurface::setPalette(myFullPalette);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void FrameBuffer::stateChanged(EventHandlerState state)
{
  // Make sure any onscreen messages are removed
  myMsg.enabled = false;
  myMsg.counter = 0;

  update(true); // force full update
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
string FrameBuffer::getDisplayKey()
{
  // save current window's display and position
  switch(myBufferType)
  {
    case BufferType::Launcher:
      return "launcherdisplay";

    case BufferType::Emulator:
      return "display";

  #ifdef DEBUGGER_SUPPORT
    case BufferType::Debugger:
      return "dbg.display";
  #endif

    default:
      return "";
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
string FrameBuffer::getPositionKey()
{
  // save current window's display and position
  switch(myBufferType)
  {
    case BufferType::Launcher:
      return "launcherpos";

    case BufferType::Emulator:
      return  "windowedpos";

  #ifdef DEBUGGER_SUPPORT
    case BufferType::Debugger:
      return "dbg.pos";
  #endif

    default:
      return "";
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void FrameBuffer::saveCurrentWindowPosition()
{
  myOSystem.settings().setValue(
      getDisplayKey(), myBackend->getCurrentDisplayIndex());
  if(myBackend->isCurrentWindowPositioned())
    myOSystem.settings().setValue(
        getPositionKey(), myBackend->getCurrentWindowPos());
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void FrameBuffer::setFullscreen(bool enable)
{
#ifdef WINDOWED_SUPPORT
  // Switching between fullscreen and windowed modes will invariably mean
  // that the 'window' resolution changes.  Currently, dialogs are not
  // able to resize themselves when they are actively being shown
  // (they would have to be closed and then re-opened, etc).
  // For now, we simply disallow screen switches in such modes
  switch(myOSystem.eventHandler().state())
  {
    case EventHandlerState::EMULATION:
    case EventHandlerState::PAUSE:
      break; // continue with processing (aka, allow a mode switch)
    case EventHandlerState::DEBUGGER:
    case EventHandlerState::LAUNCHER:
      if(myOSystem.eventHandler().overlay().baseDialogIsActive())
        break; // allow a mode switch when there is only one dialog
      [[fallthrough]];
    default:
      return;
  }

  myOSystem.settings().setValue("fullscreen", enable);
  saveCurrentWindowPosition();
  applyVideoMode();
#endif
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void FrameBuffer::toggleFullscreen(bool toggle)
{
  switch(myOSystem.eventHandler().state())
  {
    case EventHandlerState::LAUNCHER:
    case EventHandlerState::EMULATION:
    case EventHandlerState::PAUSE:
    case EventHandlerState::DEBUGGER:
    {
      const bool isFullscreen = toggle ? !fullScreen() : fullScreen();
      setFullscreen(isFullscreen);

      if(myBufferType != BufferType::Launcher)
      {
        ostringstream msg;
        msg << "Fullscreen ";
        if(isFullscreen)
          msg << "enabled (" << myBackend->refreshRate() << " Hz, ";
        else
          msg << "disabled (";
        msg << "Zoom " << myActiveVidMode.zoom * 100 << "%)";

        showMessage(msg.str());
      }
      break;
    }
    default:
      break;
  }
}

#ifdef ADAPTABLE_REFRESH_SUPPORT
// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void FrameBuffer::toggleAdaptRefresh(bool toggle)
{
  bool isAdaptRefresh = myOSystem.settings().getInt("tia.fs_refresh");

  if(toggle)
    isAdaptRefresh = !isAdaptRefresh;

  if(myBufferType == BufferType::Emulator)
  {
    if(toggle)
    {
      myOSystem.settings().setValue("tia.fs_refresh", isAdaptRefresh);
      // issue a complete framebuffer re-initialization
      myOSystem.createFrameBuffer();
    }

    ostringstream msg;

    msg << "Adapt refresh rate ";
    msg << (isAdaptRefresh ? "enabled" : "disabled");
    msg << " (" << myBackend->refreshRate() << " Hz)";

    showMessage(msg.str());
  }
}
#endif

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void FrameBuffer::changeOverscan(int direction)
{
  if (fullScreen())
  {
    int oldOverscan = myOSystem.settings().getInt("tia.fs_overscan");
    int overscan = BSPF::clamp(oldOverscan + direction, 0, 10);

    if (overscan != oldOverscan)
    {
      myOSystem.settings().setValue("tia.fs_overscan", overscan);

      // issue a complete framebuffer re-initialization
      myOSystem.createFrameBuffer();
    }

    ostringstream val;
    if(overscan)
      val << (overscan > 0 ? "+" : "" ) << overscan << "%";
    else
      val << "Off";
    myOSystem.frameBuffer().showMessage("Overscan", val.str(), overscan, 0, 10);
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void FrameBuffer::switchVideoMode(int direction)
{
  // Only applicable when in TIA/emulation mode
  if(!myOSystem.eventHandler().inTIAMode())
    return;

  if(!fullScreen())
  {
    // Windowed TIA modes support variable zoom levels
    float zoom = myOSystem.settings().getFloat("tia.zoom");
    if(direction == +1)       zoom += ZOOM_STEPS;
    else if(direction == -1)  zoom -= ZOOM_STEPS;

    // Make sure the level is within the allowable desktop size
    zoom = BSPF::clampw(zoom, supportedTIAMinZoom(), myTIAMaxZoom);
    myOSystem.settings().setValue("tia.zoom", zoom);
  }
  else
  {
    // In fullscreen mode, there are only two modes, so direction
    // is irrelevant
    if(direction == +1 || direction == -1)
    {
      bool stretch = myOSystem.settings().getBool("tia.fs_stretch");
      myOSystem.settings().setValue("tia.fs_stretch", !stretch);
    }
  }

  saveCurrentWindowPosition();
  if(applyVideoMode() == FBInitStatus::Success)
  {
    if(fullScreen())
      showMessage(myActiveVidMode.description);
    else
      showMessage("Zoom", myActiveVidMode.description, myActiveVidMode.zoom,
                  supportedTIAMinZoom(), myTIAMaxZoom);
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
FBInitStatus FrameBuffer::applyVideoMode()
{
  // Update display size, in case windowed/fullscreen mode has changed
  const Settings& s = myOSystem.settings();
  if(s.getBool("fullscreen"))
  {
    Int32 fsIndex = std::max(myBackend->getCurrentDisplayIndex(), 0);
    myVidModeHandler.setDisplaySize(myFullscreenDisplays[fsIndex], fsIndex);
  }
  else
    myVidModeHandler.setDisplaySize(myAbsDesktopSize);

  const bool inTIAMode = myOSystem.eventHandler().inTIAMode();

  // Build the new mode based on current settings
  const VideoModeHandler::Mode& mode = myVidModeHandler.buildMode(s, inTIAMode);
  if(mode.imageR.size() > mode.screenS)
    return FBInitStatus::FailTooLarge;

  // Changing the video mode can take some time, during which the last
  // sound played may get 'stuck'
  // So we mute the sound until the operation completes
  bool oldMuteState = myOSystem.sound().mute(true);
  FBInitStatus status = FBInitStatus::FailNotSupported;

  if(myBackend->setVideoMode(mode,
      myOSystem.settings().getInt(getDisplayKey()),
      myOSystem.settings().getPoint(getPositionKey()))
    )
  {
    myActiveVidMode = mode;
    status = FBInitStatus::Success;

    // Did we get the requested fullscreen state?
    myOSystem.settings().setValue("fullscreen", fullScreen());

    // Inform TIA surface about new mode, and update TIA settings
    if(inTIAMode)
    {
      myTIASurface->initialize(myOSystem.console(), myActiveVidMode);
      if(fullScreen())
        myOSystem.settings().setValue("tia.fs_stretch",
          myActiveVidMode.stretch == VideoModeHandler::Mode::Stretch::Fill);
      else
        myOSystem.settings().setValue("tia.zoom", myActiveVidMode.zoom);
    }

    resetSurfaces();
    setCursorState();
  }
  else
    Logger::error("ERROR: Couldn't initialize video subsystem");

  // Restore sound settings
  myOSystem.sound().mute(oldMuteState);

  return status;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
float FrameBuffer::maxWindowZoom(uInt32 baseWidth, uInt32 baseHeight) const
{
  float multiplier = 1;
  for(;;)
  {
    // Figure out the zoomed size of the window
    uInt32 width  = baseWidth * multiplier;
    uInt32 height = baseHeight * multiplier;

    if((width > myAbsDesktopSize.w) || (height > myAbsDesktopSize.h))
      break;

    multiplier += ZOOM_STEPS;
  }
  return multiplier > 1 ? multiplier - ZOOM_STEPS : 1;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void FrameBuffer::setCursorState()
{
  // Always grab mouse in emulation (if enabled) and emulating a controller
  // that always uses the mouse
  bool emulation =
      myOSystem.eventHandler().state() == EventHandlerState::EMULATION;
  bool analog = myOSystem.hasConsole() ?
      (myOSystem.console().leftController().isAnalog() ||
       myOSystem.console().rightController().isAnalog()) : false;
  bool usesLightgun = emulation && myOSystem.hasConsole() ?
    myOSystem.console().leftController().type() == Controller::Type::Lightgun ||
    myOSystem.console().rightController().type() == Controller::Type::Lightgun : false;
  bool alwaysUseMouse = BSPF::equalsIgnoreCase("always", myOSystem.settings().getString("usemouse"));

  // Show/hide cursor in UI/emulation mode based on 'cursor' setting
  int cursor = myOSystem.settings().getInt("cursor");
  // always enable cursor in lightgun games
  if (usesLightgun && !myGrabMouse)
    cursor |= 1;  // +Emulation

  switch(cursor)
  {
    case 0:                   // -UI, -Emulation
      showCursor(false);
      break;
    case 1:
      showCursor(emulation);  //-UI, +Emulation
      myGrabMouse = false; // disable grab while cursor is shown in emulation
      break;
    case 2:                   // +UI, -Emulation
      showCursor(!emulation);
      break;
    case 3:
      showCursor(true);       // +UI, +Emulation
      myGrabMouse = false; // disable grab while cursor is shown in emulation
      break;
  }

  myBackend->grabMouse(emulation && (analog || alwaysUseMouse) && myGrabMouse);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void FrameBuffer::enableGrabMouse(bool enable)
{
  myGrabMouse = enable;
  setCursorState();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void FrameBuffer::toggleGrabMouse()
{
  const bool oldState = myGrabMouse;

  myGrabMouse = !myGrabMouse;
  setCursorState();
  myOSystem.settings().setValue("grabmouse", myGrabMouse);
  myOSystem.frameBuffer().showMessage(oldState != myGrabMouse ? myGrabMouse
                                      ? "Grab mouse enabled" : "Grab mouse disabled"
                                      : "Grab mouse not allowed while cursor shown");
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
/*
  Palette is defined as follows:
    *** Base colors ***
    kColor            Normal foreground color (non-text)
    kBGColor          Normal background color (non-text)
    kBGColorLo        Disabled background color dark (non-text)
    kBGColorHi        Disabled background color light (non-text)
    kShadowColor      Item is disabled
    *** Text colors ***
    kTextColor        Normal text color
    kTextColorHi      Highlighted text color
    kTextColorEm      Emphasized text color
    kTextColorInv     Color for selected text
    *** UI elements (dialog and widgets) ***
    kDlgColor         Dialog background
    kWidColor         Widget background
    kWidColorHi       Widget highlight color
    kWidFrameColor    Border for currently selected widget
    *** Button colors ***
    kBtnColor         Normal button background
    kBtnColorHi       Highlighted button background
    kBtnBorderColor,
    kBtnBorderColorHi,
    kBtnTextColor     Normal button font color
    kBtnTextColorHi   Highlighted button font color
    *** Checkbox colors ***
    kCheckColor       Color of 'X' in checkbox
    *** Scrollbar colors ***
    kScrollColor      Normal scrollbar color
    kScrollColorHi    Highlighted scrollbar color
    *** Debugger colors ***
    kDbgChangedColor      Background color for changed cells
    kDbgChangedTextColor  Text color for changed cells
    kDbgColorHi           Highlighted color in debugger data cells
    kDbgColorRed          Red color in debugger
    *** Slider colors ***
    kSliderColor          Enabled slider
    kSliderColorHi        Focussed slider
    kSliderBGColor        Enabled slider background
    kSliderBGColorHi      Focussed slider background
    kSliderBGColorLo      Disabled slider background
    *** Other colors ***
    kColorInfo            TIA output position color
    kColorTitleBar        Title bar color
    kColorTitleText       Title text color
    kColorTitleBarLo      Disabled title bar color
    kColorTitleTextLo     Disabled title text color
*/
UIPaletteArray FrameBuffer::ourStandardUIPalette = {
  { 0x686868, 0x000000, 0xa38c61, 0xdccfa5, 0x404040,           // base
    0x000000, 0xac3410, 0x9f0000, 0xf0f0cf,                     // text
    0xc9af7c, 0xf0f0cf, 0xd55941, 0xc80000,                     // UI elements
    0xac3410, 0xd55941, 0x686868, 0xdccfa5, 0xf0f0cf, 0xf0f0cf, // buttons
    0xac3410,                                                   // checkbox
    0xac3410, 0xd55941,                                         // scrollbar
    0xc80000, 0xffff80, 0xc8c8ff, 0xc80000,                     // debugger
    0xac3410, 0xd55941, 0xdccfa5, 0xf0f0cf, 0xa38c61,           // slider
    0xffffff, 0xac3410, 0xf0f0cf, 0x686868, 0xdccfa5            // other
  }
};

UIPaletteArray FrameBuffer::ourClassicUIPalette = {
  { 0x686868, 0x000000, 0x404040, 0x404040, 0x404040,           // base
    0x20a020, 0x00ff00, 0xc80000, 0x000000,                     // text
    0x000000, 0x000000, 0x00ff00, 0xc80000,                     // UI elements
    0x000000, 0x000000, 0x686868, 0x00ff00, 0x20a020, 0x00ff00, // buttons
    0x20a020,                                                   // checkbox
    0x20a020, 0x00ff00,                                         // scrollbar
    0xc80000, 0x00ff00, 0xc8c8ff, 0xc80000,                     // debugger
    0x20a020, 0x00ff00, 0x404040, 0x686868, 0x404040,           // slider
    0x00ff00, 0x20a020, 0x000000, 0x686868, 0x404040            // other
  }
};

UIPaletteArray FrameBuffer::ourLightUIPalette = {
  { 0x808080, 0x000000, 0xc0c0c0, 0xe1e1e1, 0x333333,           // base
    0x000000, 0xBDDEF9, 0x0078d7, 0x000000,                     // text
    0xf0f0f0, 0xffffff, 0x0078d7, 0x0f0f0f,                     // UI elements
    0xe1e1e1, 0xe5f1fb, 0x808080, 0x0078d7, 0x000000, 0x000000, // buttons
    0x333333,                                                   // checkbox
    0xc0c0c0, 0x808080,                                         // scrollbar
    0xffc0c0, 0x000000, 0xe00000, 0xc00000,                     // debugger
    0x333333, 0x0078d7, 0xc0c0c0, 0xffffff, 0xc0c0c0,           // slider 0xBDDEF9| 0xe1e1e1 | 0xffffff
    0xffffff, 0x333333, 0xf0f0f0, 0x808080, 0xc0c0c0            // other
  }
};

UIPaletteArray FrameBuffer::ourDarkUIPalette = {
  { 0x646464, 0xc0c0c0, 0x3c3c3c, 0x282828, 0x989898,           // base
    0xc0c0c0, 0x1567a5, 0x0059a3, 0xc0c0c0,                     // text
    0x202020, 0x000000, 0x0059a3, 0xb0b0b0,                     // UI elements
    0x282828, 0x00467f, 0x646464, 0x0059a3, 0xc0c0c0, 0xc0c0c0, // buttons
    0x989898,                                                   // checkbox
    0x3c3c3c, 0x646464,                                         // scrollbar
    0x7f2020, 0xc0c0c0, 0xe00000, 0xc00000,                     // debugger
    0x989898, 0x0059a3, 0x3c3c3c, 0x000000, 0x3c3c3c,           // slider
    0x000000, 0x989898, 0x202020, 0x646464, 0x3c3c3c            // other
  }
};
