/*
 * GDB Debugger Support
 *
 * Copyright 1999 John Birch <jbb@kdevelop.org>
 * Copyright 2007 Hamish Rodda <rodda@kde.org>
 * Copyright 2008 Vladimir Prus <ghost@cs.msu.su>
 * Copyright 2009 Aleix Pol <aleixpol@kde.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "stackmanager.h"

#include <klocale.h>
#include <kdebug.h>
#include <kglobalsettings.h>

#include "gdbparser.h"
#include "gdbcommand.h"
#include "gdbcontroller.h"

#include <debugger/util/treeitem.h>
#include <debugger/util/treemodel.h>

// #include "modeltest.h"

using namespace GDBMI;
using namespace GDBDebugger;
using namespace KDevelop;

namespace GDBDebugger {

QString get_function_or_address(const GDBMI::Value &frame)
{
    if (frame.hasField("func"))
        return frame["func"].literal();
    else
        return frame["addr"].literal();
}

QPair<QString, int> get_source(const GDBMI::Value &frame)
{
    QPair<QString, int> ret(QString(), 0);
    if (frame.hasField("file"))
        ret=qMakePair(frame["file"].literal(), frame["line"].toInt());
    else if (frame.hasField("from"))
        ret.first=frame["from"].literal();
    
    return ret;
}

Frame::Frame(FramesModel* model, Thread* parent, const GDBMI::Value& frame)
    : FrameItem(model)
{
    updateSelf(frame);
}

void Frame::updateSelf(const GDBMI::Value& frame)
{
    QPair<QString, int> loc=get_source(frame);
    setInformation(frame["level"].toInt(), get_function_or_address(frame), loc.first, loc.second);
}

Thread::Thread(KDevelop::StackModel* model, GDBDebugger::GDBController* controller, const GDBMI::Value& thread)
    : ThreadItem(model), controller_(controller)
{
    id_ = thread["id"].toInt();

    updateSelf(thread, true);
    setHasMoreInitial(false);
}

void Thread::updateSelf(const GDBMI::Value& thread, bool initial)
{
    const GDBMI::Value& frame = thread["frame"];
    setInformation(frame["level"].toInt(), get_function_or_address(frame));
    if (!initial)
        reportChange();

    if (isExpanded())
        fetchMoreChildren_1(true);
}

void Thread::fetchMoreChildren()
{
    fetchMoreChildren_1(false);
}

void Thread::fetchMoreChildren_1(bool clear)
{
    /* We always ask GDB for:
       - the last frame we already have
       - 'step' more frames
       - one more frame

       We ask for the last present frame so that GDB does not give fits
       if the last frame we have is exactly the last frame target has.
       We ask for one more frame so that we know if there are more
       frames.  */
    int now;
    if (clear)
        now = 0;
    else
        now = framesModel()->framesCount();
    int next = now + step + 1;
    if (clear)
        now = 0;
    QString arg = QString("%1 %2").arg(now).arg(next);

    GDBCommand *c = new GDBCommand(StackListFrames, arg,
                                   this,
                                   &Thread::handleFrameList);
    c->setThread(id_);
    controller_->addCommand(c);
}

void Thread::handleFrameList(const GDBMI::ResultRecord& r)
{
    const GDBMI::Value& stack = r["stack"];
    int first = stack[0]["level"].toInt();
    if (first == 0)
    {
        /* For smooth update, after stepping we update first
           block of frames without previously clearing the
           existing ones.
           Also note that we ignore the first stack frame
           here.  */
        int i;
        for (i = 0; i < step && i< stack.size(); ++i)
        {
            if (i < framesModel()->framesCount())
            {
                Frame *f = static_cast<Frame *>(child(i));
                f->updateSelf(stack[i]);
            }
            else
            {
                framesModel()->addFrame(new Frame(framesModel(), this, stack[i]));
            }
        }
        while (i < framesModel()->framesCount())
            removeChild(i);
        framesModel()->setHasMoreFrames(stack.size() > step);
    }
    else
    {
        if (first != framesModel()->framesCount())
        {
            kDebug(9012) << "Got wrong frames\n";
            return;
        }
        
        for (int i = 0; i < step && i<stack.size(); ++i)
            framesModel()->addFrame(new Frame(framesModel(), this, stack[i]));
        
        framesModel()->setHasMoreFrames(stack.size() > step);
    }
}

class DebugUniverse : public TreeItem
{
    Q_OBJECT
public:
    DebugUniverse(StackModel* model, GDBController *controller,
                  StackManager *stackManager)
    : TreeItem(model), controller_(controller), stackManager_(stackManager), mModel(model)
    {}

    void update()
    {
        if (!controller_->stateIsOn(s_dbgNotStarted)) {
            controller_->addCommand(
                new GDBCommand(ThreadInfo, "",
                            this,
                            &DebugUniverse::handleThreadInfo));
        }
    }

    void fetchMoreChildren() {}

    using TreeItem::clear;

private:

    void handleThreadInfo(const GDBMI::ResultRecord& r)
    {
        const GDBMI::Value& threads = r["threads"];
        int current_id = r["current-thread-id"].toInt();

        // Collect the set of ids that are present in
        // target now.
        QSet<int> present;
        for (int i = 0; i < threads.size(); ++i)
            present.insert(threads[i]["id"].toInt());

        // Remove threads that are no longer present
        for (int i = 0; i < childItems.size(); ++i)
        {
            Thread* t = static_cast<Thread *>(child(i));
            if (!present.contains(t->id()))
                removeChild(i);
        }

        // Traverse GDB threads in backward order -- since GDB
        // reports them in backward order. We want UI to
        // show thread IDs in the natural order.
        // FIXME: make the code independent of whatever craziness
        // gdb might have tomorrow.

        int gidx = threads.size()-1;
        int kidx = 0;

        for (; gidx >= 0; --gidx)
        {
            bool updated = false;
            if (kidx < childItems.size())
            {
                Thread* t = static_cast<Thread *>(child(kidx));
                if (threads[gidx]["id"].toInt() == t->id())
                {
                    t->updateSelf(threads[gidx]);
                    updated = true;
                    ++kidx;
                }
            }
            if (!updated)
                break;
        }

        for (; gidx >= 0; --gidx)
            appendChild(new Thread(mModel, controller_, threads[gidx]));

        for (int i = 0; i < childItems.size(); ++i)
        {
            Thread* t = static_cast<Thread *>(child(i));
            if (t->id() == current_id)
            {
                emit stackManager_
                    ->selectThreadReally(mModel->indexForItem(t, 0));
            }
        }

    }

    GDBController* controller_;
    StackManager* stackManager_;
    StackModel* mModel;
};
}

void GDBDebugger::Thread::clicked()
{
    KDevelop::ThreadItem::clicked();
}

StackManager::StackManager(GDBController* controller)
  : KDevelop::StackModel(),
    controller_(controller)
{
    universe_ = new DebugUniverse(this, controller, this);
    setRootItem(universe_);

    // new ModelTest(model_, this);

    // FIXME: maybe, all debugger components should derive from
    // a base class that does this connect.
    connect(controller, SIGNAL(event(event_t)),
            this,       SLOT(slotEvent(event_t)));
}

StackManager::~StackManager()
{
}

void StackManager::update()
{
    if (m_autoUpdate)
        universe_->update();
}

void StackManager::slotEvent(event_t e)
{
    switch(e)
    {
        case program_state_changed:

            if (m_autoUpdate)
                update();

            break;

         case thread_or_frame_changed:
            break;

        case program_exited:
        case debugger_exited:
            universe_->clear();
            break;

        case debugger_busy:
        case debugger_ready:
        case program_running:
        case connected_to_program:
            break;
    }
}

#include "moc_stackmanager.cpp"
#include "stackmanager.moc"
