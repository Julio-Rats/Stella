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

#include "OSystem.hxx"
#include "EventHandler.hxx"
#include "Event.hxx"
#include "Console.hxx"
#include "System.hxx"
#include "TIA.hxx"
#include "FrameBuffer.hxx"
#include "AtariVox.hxx"
#include "Driving.hxx"
#include "Joystick.hxx"
#include "SaveKey.hxx"
#include "QuadTari.hxx"

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
QuadTari::QuadTari(Jack jack, OSystem& osystem, const System& system, const Properties& properties)
  : Controller(jack, osystem.eventHandler().event(), system,
               Controller::Type::QuadTari),
    myOSystem(osystem)
{
  string first, second;
  Controller::Type firstType = Controller::Type::Joystick,
    secondType = Controller::Type::Joystick;

  if(jack == Controller::Jack::Left)
  {
    first = properties.get(PropType::Controller_Left1);
    second = properties.get(PropType::Controller_Left2);
  }
  else
  {
    first = properties.get(PropType::Controller_Right1);
    second = properties.get(PropType::Controller_Right2);
  }

  if(!first.empty())
    firstType = Controller::getType(first);
  if(!second.empty())
    secondType = Controller::getType(second);

  myFirstController = addController(firstType, false);
  mySecondController = addController(secondType, true);

  // QuadTari auto detection setting
  setPin(AnalogPin::Five, MIN_RESISTANCE);
  setPin(AnalogPin::Nine, MAX_RESISTANCE);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
unique_ptr<Controller> QuadTari::addController(const Controller::Type type, bool second)
{
  switch(type)
  {
    case Controller::Type::AtariVox:
    {
      FilesystemNode nvramfile = myOSystem.nvramDir();
      nvramfile /= "atarivox_eeprom.dat";
      Controller::onMessageCallback callback = [&os = myOSystem](const string& msg) {
        bool devSettings = os.settings().getBool("dev.settings");
        if(os.settings().getBool(devSettings ? "dev.eepromaccess" : "plr.eepromaccess"))
          os.frameBuffer().showMessage(msg);
      };
      return make_unique<AtariVox>(myJack, myEvent, mySystem,
                                   myOSystem.settings().getString("avoxport"),
                                   nvramfile, callback); // no alternative mapping here
    }
    case Controller::Type::Driving:
      return make_unique<Driving>(myJack, myEvent, mySystem, second);

    case Controller::Type::SaveKey:
    {
      FilesystemNode nvramfile = myOSystem.nvramDir();
      nvramfile /= "savekey_eeprom.dat";
      Controller::onMessageCallback callback = [&os = myOSystem](const string& msg) {
        bool devSettings = os.settings().getBool("dev.settings");
        if(os.settings().getBool(devSettings ? "dev.eepromaccess" : "plr.eepromaccess"))
          os.frameBuffer().showMessage(msg);
      };
      return make_unique<SaveKey>(myJack, myEvent, mySystem,
                                  nvramfile, callback); // no alternative mapping here
    }
    default:
      // fall back to good old Joystick
      return make_unique<Joystick>(myJack, myEvent, mySystem, second);
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool QuadTari::read(DigitalPin pin)
{
  // We need to override the Controller::read() method, since the QuadTari
  // can switch the controller multiple times per frame
  // (we can't just read 60 times per second in the ::update() method)

  constexpr int MIN_CYCLES = 20 * 76; // minimal cycles required for stable input switch (TODO: define cycles)
  bool readFirst;

  if(mySystem.tia().dumpPortsCycles() < MIN_CYCLES)
    // Random controller if read too soon after dump ports changed
    readFirst = mySystem.randGenerator().next() % 2;
  else
    // If bit 7 of VBlank is not set, read first, else second controller
    readFirst = !(mySystem.tia().registerValue(VBLANK) & 0x80);

  if(readFirst)
    return myFirstController->read(pin);
  else
    return mySecondController->read(pin);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void QuadTari::update()
{
  myFirstController->update();
  mySecondController->update();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
string QuadTari::name() const
{
  return "QuadTari (" + myFirstController->name() + "/" + mySecondController->name() + ")";
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool QuadTari::isAnalog() const
{
  // For now, use mouse for first controller only
  return myFirstController->isAnalog();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool QuadTari::setMouseControl(
    Controller::Type xtype, int xid, Controller::Type ytype, int yid)
{
  // Use mouse for first controller only
  if(xtype == Controller::Type::QuadTari && ytype == Controller::Type::QuadTari)
    return myFirstController->setMouseControl(myFirstController->type(), xid,
                                              myFirstController->type(), yid);
  else
    // required for creating the MouseControl mode list
    return myFirstController->setMouseControl(xtype, xid, ytype, yid);
}
