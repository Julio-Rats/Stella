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

#include "Dialog.hxx"
#include "StellaKeys.hxx"
#include "FBSurface.hxx"
#include "Font.hxx"
#include "OSystem.hxx"
#include "EventHandler.hxx"
#include "UndoHandler.hxx"
#include "EditableWidget.hxx"

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
EditableWidget::EditableWidget(GuiObject* boss, const GUI::Font& font,
                               int x, int y, int w, int h, const string& str)
  : Widget(boss, font, x, y, w, h),
    CommandSender(boss),
    _editString(str),
    _filter([](char c) { return isprint(c) && c != '\"'; })
{
  _bgcolor = kWidColor;
  _bgcolorhi = kWidColor;
  _bgcolorlo = kDlgColor;
  _textcolor = kTextColor;
  _textcolorhi = kTextColor;

  myUndoHandler = make_unique<UndoHandler>();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void EditableWidget::setText(const string& str, bool)
{
  // Filter input string
  _editString = "";
  for(char c: str)
    if(_filter(tolower(c)))
      _editString.push_back(c);

  myUndoHandler->reset();
  myUndoHandler->doo(_editString);

  _caretPos = int(_editString.size());
  _selectSize = 0;

  _editScrollOffset = (_font.getStringWidth(_editString) - (getEditRect().w()));
  if (_editScrollOffset < 0)
    _editScrollOffset = 0;

  setDirty();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void EditableWidget::setEditable(bool editable, bool hiliteBG)
{
  _editable = editable;
  if(_editable)
  {
    setFlags(Widget::FLAG_WANTS_RAWDATA | Widget::FLAG_RETAIN_FOCUS);
    _bgcolor = kWidColor;
  }
  else
  {
    clearFlags(Widget::FLAG_WANTS_RAWDATA | Widget::FLAG_RETAIN_FOCUS);
    _bgcolor = hiliteBG ? kBGColorHi : kWidColor;
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void EditableWidget::lostFocusWidget()
{
  myUndoHandler->reset();
  _selectSize = 0;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool EditableWidget::tryInsertChar(char c, int pos)
{
  if(_filter(tolower(c)))
  {
    killSelectedText();
    myUndoHandler->doChar(); // aggregate single chars
    _editString.insert(pos, 1, c);
    return true;
  }
  return false;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool EditableWidget::handleText(char text)
{
  if(!_editable)
    return true;

  if(tryInsertChar(text, _caretPos))
  {
    _caretPos++;
    sendCommand(EditableWidget::kChangedCmd, 0, _id);
    setDirty();
    return true;
  }
  return false;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool EditableWidget::handleKeyDown(StellaKey key, StellaMod mod)
{
  if(!_editable)
    return true;

  bool handled = true;
  Event::Type event = instance().eventHandler().eventForKey(EventMode::kEditMode, key, mod);

  switch(event)
  {
    case Event::MoveLeftChar:
      if(_selectSize)
        handled = setCaretPos(selectStartPos());
      else if(_caretPos > 0)
        handled = setCaretPos(_caretPos - 1);
      _selectSize = 0;
      break;

    case Event::MoveRightChar:
      if(_selectSize)
        handled = setCaretPos(selectEndPos());
      else if(_caretPos < int(_editString.size()))
        handled = setCaretPos(_caretPos + 1);
      _selectSize = 0;
      break;

    case Event::MoveLeftWord:
      handled = moveWord(-1, false);
      _selectSize = 0;
      break;

    case Event::MoveRightWord:
      handled = moveWord(+1, false);
      _selectSize = 0;
      break;

    case Event::MoveHome:
      handled = setCaretPos(0);
      _selectSize = 0;
      break;

    case Event::MoveEnd:
      handled = setCaretPos(int(_editString.size()));
      _selectSize = 0;
      break;

    case Event::SelectLeftChar:
      if(_caretPos > 0)
        handled = moveCaretPos(-1);
      break;

    case Event::SelectRightChar:
      if(_caretPos < int(_editString.size()))
        handled = moveCaretPos(+1);
      break;

    case Event::SelectLeftWord:
      handled = moveWord(-1, true);
      break;

    case Event::SelectRightWord:
      handled = moveWord(+1, true);
      break;

    case Event::SelectHome:
      handled = moveCaretPos(-_caretPos);
      break;

    case Event::SelectEnd:
      handled = moveCaretPos(int(_editString.size()) - _caretPos);
      break;

    case Event::SelectAll:
      if(setCaretPos(int(_editString.size())))
        _selectSize = -int(_editString.size());
      break;

    case Event::Backspace:
      handled = killSelectedText();
      if(!handled)
        handled = killChar(-1);
      if(handled)
        sendCommand(EditableWidget::kChangedCmd, key, _id);
      break;

    case Event::Delete:
      handled = killSelectedText();
      if(!handled)
        handled = killChar(+1);
      if(handled)
        sendCommand(EditableWidget::kChangedCmd, key, _id);
      break;

    case Event::DeleteLeftWord:
      handled = killWord(-1);
      if(handled)
        sendCommand(EditableWidget::kChangedCmd, key, _id);
      break;

    case Event::DeleteRightWord:
      handled = killWord(+1);
      if(handled)
        sendCommand(EditableWidget::kChangedCmd, key, _id);
      break;

    case Event::DeleteEnd:
      handled = killLine(+1);
      if(handled)
        sendCommand(EditableWidget::kChangedCmd, key, _id);
      break;

    case Event::DeleteHome:
      handled = killLine(-1);
      if(handled)
        sendCommand(EditableWidget::kChangedCmd, key, _id);
      break;

    case Event::Cut:
      handled = cutSelectedText();
      if(handled)
        sendCommand(EditableWidget::kChangedCmd, key, _id);
      break;

    case Event::Copy:
      handled = copySelectedText();
      break;

    case Event::Paste:
      handled = pasteSelectedText();
      if(handled)
        sendCommand(EditableWidget::kChangedCmd, key, _id);
      break;

    case Event::Undo:
    case Event::Redo:
    {
      string oldString = _editString;

      myUndoHandler->endChars(_editString);
      // Reverse Y and Z for QWERTZ keyboards
      if(event == Event::Redo)
        handled = myUndoHandler->redo(_editString);
      else
        handled = myUndoHandler->undo(_editString);

      if(handled)
      {
        // Put caret at last difference
        myUndoHandler->lastDiff(_editString, oldString);
        _caretPos = myUndoHandler->lastDiff(_editString, oldString);
        _selectSize = 0;
        sendCommand(EditableWidget::kChangedCmd, key, _id);
      }
      break;
    }

    case Event::EndEdit:
      // confirm edit and exit editmode
      endEditMode();
      sendCommand(EditableWidget::kAcceptCmd, 0, _id);
      break;

    case Event::AbortEdit:
      abortEditMode();
      sendCommand(EditableWidget::kCancelCmd, 0, _id);
      break;

    default:
      handled = false;
      break;
  }

  if(handled)
  {
    myUndoHandler->endChars(_editString);
    setDirty();
  }

  return handled;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
int EditableWidget::getCaretOffset() const
{
  int caretOfs = 0;
  for (int i = 0; i < _caretPos; i++)
    caretOfs += _font.getCharWidth(_editString[i]);

  caretOfs -= _editScrollOffset;

  return caretOfs;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void EditableWidget::drawCaretSelection()
{
  // Only draw if item is visible
  if (!_editable || !isVisible() || !_boss->isVisible() || !_hasFocus)
    return;

  const Common::Rect& editRect = getEditRect();
  int x = editRect.x();
  int y = editRect.y();

  x += getCaretOffset();

  x += _x;
  y += _y;

  FBSurface& s = _boss->dialog().surface();
  s.vLine(x, y + 2, y + editRect.h() - 2, kTextColorHi);
  s.vLine(x-1, y + 2, y + editRect.h() - 2, kTextColorHi);

  if(_selectSize)
  {
    string text = selectString();
    x = editRect.x();
    y = editRect.y();
    int w = editRect.w();
    int h = editRect.h();
    int wt = int(text.length()) * _font.getMaxCharWidth() + 1;
    int dx = selectStartPos() * _font.getMaxCharWidth() - _editScrollOffset;

    if(dx < 0)
    {
      // selected text starts left of displayed rect
      text = text.substr(-(dx - 1) / _font.getMaxCharWidth());
      wt += dx;
      dx = 0;
    }
    else
      x += dx;
    // limit selection to the right of displayed rect
    w = std::min(w - dx + 1, wt);

    x += _x;
    y += _y;

    s.fillRect(x - 1, y + 1, w + 1, h - 3, kTextColorHi);
    s.drawString(_font, text, x, y + 1, w, h,
                 kTextColorInv, TextAlign::Left, 0, false);
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool EditableWidget::setCaretPos(int newPos)
{
  assert(newPos >= 0 && newPos <= int(_editString.size()));
  _caretPos = newPos;

  return adjustOffset();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool EditableWidget::moveCaretPos(int direction)
{
  if(setCaretPos(_caretPos + direction))
  {
    _selectSize -= direction;
    return true;
  }
  return false;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool EditableWidget::adjustOffset()
{
  // check if the caret is still within the textbox; if it isn't,
  // adjust _editScrollOffset

  // For some reason (differences in ScummVM event handling??),
  // this method should always return true.

  int caretOfs = getCaretOffset();
  const int editWidth = getEditRect().w();

  if (caretOfs < 0)
  {
    // scroll left
    _editScrollOffset += caretOfs;
  }
  else if (caretOfs >= editWidth)
  {
    // scroll right
    _editScrollOffset -= (editWidth - caretOfs);
  }
  else if (_editScrollOffset > 0)
  {
    const int strWidth = _font.getStringWidth(_editString);
    if (strWidth - _editScrollOffset < editWidth)
    {
      // scroll right
      _editScrollOffset = (strWidth - editWidth);
      if (_editScrollOffset < 0)
        _editScrollOffset = 0;
    }
  }

  return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
int EditableWidget::scrollOffset()
{
  return _editable ? -_editScrollOffset : 0;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool EditableWidget::killChar(int direction, bool addEdit)
{
  bool handled = false;

  if(direction == -1)      // Delete previous character (backspace)
  {
    if(_caretPos > 0)
    {
      _caretPos--;
      if(_selectSize < 0)
        _selectSize++;
      handled = true;
    }
  }
  else if(direction == 1)  // Delete next character (delete)
  {
    if(_caretPos < int(_editString.size()))
    {
      if(_selectSize > 0)
        _selectSize--;
      handled = true;
    }
  }

  if(handled)
  {
    myUndoHandler->endChars(_editString);
    _editString.erase(_caretPos, 1);

    if(addEdit)
      myUndoHandler->doo(_editString);
  }

  return handled;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool EditableWidget::killLine(int direction)
{
  int count = 0;

  if(direction == -1)  // erase from current position to beginning of line
    count = _caretPos;
  else if(direction == +1)  // erase from current position to end of line
    count = int(_editString.size()) - _caretPos;

  if(count > 0)
  {
    for(int i = 0; i < count; i++)
      killChar(direction, false);

    myUndoHandler->doo(_editString);
    return true;
  }

  return false;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool EditableWidget::killWord(int direction)
{
  bool space = true;
  int count = 0, currentPos = _caretPos;

  if(direction == -1)  // move to first character of previous word
  {
    while(currentPos > 0)
    {
      if(_editString[currentPos - 1] == ' ')
      {
        if(!space)
          break;
      }
      else
        space = false;

      currentPos--;
      count++;
    }
  }
  else if(direction == +1)  // move to first character of next word
  {
    while(currentPos < int(_editString.size()))
    {
      if(currentPos && _editString[currentPos - 1] == ' ')
      {
        if(!space)
          break;
      }
      else
        space = false;

      currentPos++;
      count++;
    }
  }

  if(count > 0)
  {
    for(int i = 0; i < count; i++)
      killChar(direction, false);

    myUndoHandler->doo(_editString);
    return true;
  }

  return false;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool EditableWidget::moveWord(int direction, bool select)
{
  bool handled = false;
  bool space = true;
  int currentPos = _caretPos;

  if(direction == -1)  // move to first character of previous word
  {
    while (currentPos > 0)
    {
      if (_editString[currentPos - 1] == ' ')
      {
        if (!space)
          break;
      }
      else
        space = false;

      currentPos--;
      if(select)
        _selectSize++;
    }
    _caretPos = currentPos;
    handled = true;
  }
  else if(direction == +1)  // move to first character of next word
  {
    while (currentPos < int(_editString.size()))
    {
      if (currentPos && _editString[currentPos - 1] == ' ')
      {
        if (!space)
          break;
      }
      else
        space = false;

      currentPos++;
      if(select)
        _selectSize--;
    }
    _caretPos = currentPos;
    handled = true;
  }

  return handled;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
const string EditableWidget::selectString() const
{
  if(_selectSize)
  {
    int caretPos = _caretPos;
    int selectSize = _selectSize;

    if(selectSize < 0)
    {
      caretPos += selectSize;
      selectSize = -selectSize;
    }
    return _editString.substr(caretPos, selectSize);
  }
  return EmptyString;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
int EditableWidget::selectStartPos()
{
  if(_selectSize < 0)
    return _caretPos + _selectSize;
  else
    return _caretPos;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
int EditableWidget::selectEndPos()
{
  if(_selectSize > 0)
    return _caretPos + _selectSize;
  else
    return _caretPos;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool EditableWidget::killSelectedText(bool addEdit)
{
  if(_selectSize)
  {
    myUndoHandler->endChars(_editString);
    if(_selectSize < 0)
    {
      _caretPos += _selectSize;
      _selectSize = -_selectSize;
    }
    _editString.erase(_caretPos, _selectSize);
    _selectSize = 0;
    if(addEdit)
      myUndoHandler->doo(_editString);
    return true;
  }
  return false;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool EditableWidget::cutSelectedText()
{
  return copySelectedText() && killSelectedText();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool EditableWidget::copySelectedText()
{
  string selected = selectString();

  // only copy if anything is selected, else keep old copied text
  if(!selected.empty())
  {
    instance().eventHandler().copyText(selected);
    return true;
  }
  return false;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool EditableWidget::pasteSelectedText()
{
  bool selected = !selectString().empty();
  string pasted;

  myUndoHandler->endChars(_editString);

  // retrieve the pasted text
  instance().eventHandler().pasteText(pasted);
  // remove the currently selected text
  killSelectedText(false);
  // insert filtered paste text instead
  ostringstream buf;
  bool lastOk = true; // only one filler char per invalid character (block)

  for(char c : pasted)
    if(_filter(tolower(c)))
    {
      buf << c;
      lastOk = true;
    }
    else
    {
      if(lastOk)
        buf << '_';
      lastOk = false;
    }

  _editString.insert(_caretPos, buf.str());
  // position cursor at the end of pasted text
  _caretPos += int(buf.str().length());

  if(selected || !pasted.empty())
  {
    myUndoHandler->doo(_editString);
    return true;
  }
  return false;
}
