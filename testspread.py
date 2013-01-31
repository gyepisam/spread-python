# Copyright (c) 2001-2005 Python Software Foundation.  All rights reserved.
#
# This code is released under the standard PSF license.
# See the file LICENSE.

import sys
import os
import time
import unittest
from distutils.util import get_platform

def setup_path():
    PLAT_SPEC = "%s-%s" % (get_platform(), sys.version[0:3])
    DIRS = [os.path.join("build", "lib.%s" % PLAT_SPEC),
            ]
    for d in DIRS:
        sys.path.insert(0, d)

setup_path()

import spread


class SpreadTest(unittest.TestCase):

    spread_name = str(spread.DEFAULT_SPREAD_PORT) + "@localhost"

    conn_counter = 0

    def _connect(self, membership=1):
        """Return a new connection"""
        c = SpreadTest.conn_counter
        SpreadTest.conn_counter += 1
        mb = spread.connect(self.spread_name, "conn%d" % c, 0, membership)
        return mb

    group_counter = 0

    def _group(self):
        """Return a new group name"""
        c = SpreadTest.group_counter
        SpreadTest.group_counter += 1
        return "group%d" % c

    def _connect_group(self, size):
        """Return a group and a list of size connections in that group.

        All the connections should have consumed all their membership
        messages.
        """
        group = self._group()
        l = [self._connect() for i in range(size)]
        for i in range(len(l)):
            c = l[i]
            c.join(group)
        # read membership messages until all receive notification of
        # the appropriate group size
        ready = []
        while l:
            for c in l[:]:
                if c.poll():
                    msg = c.receive()
                    self.assertEqual(type(msg), spread.MembershipMsgType)
                    if len(msg.members) == size:
                        ready.append(c)
                        l.remove(c)
        return group, ready

    def testSingleConnect(self):
        mbox = self._connect()
        group = self._group()
        mbox.join(group)
        while mbox.poll() == 0:
            time.sleep(0.1)
        msg = mbox.receive()

        # Should get a message indicating that we've joined the group
        self.assertEqual(msg.group, group,
                         "expected message group to match joined group")
        self.assertEqual(msg.reason, spread.CAUSED_BY_JOIN,
                         "expected message to be caused by join")
        self.assertEqual(len(msg.members), 1,
                         "expected group to have one member")
        self.assertEqual(msg.members[0], mbox.private_group,
                         "expected this mbox to be in group")
        self.assertEqual(len(msg.extra), 1,
                         "expected one mbox to cause the join")
        self.assertEqual(msg.extra[0], mbox.private_group,
                         "expected this mbox to cause the join")

        mbox.leave(group)
        # should get a self-leave message
        msg = mbox.receive()
        self.assertEqual(msg.group, group)
        self.assertEqual(msg.reason, spread.CAUSED_BY_LEAVE)
        self.assertEqual(len(msg.members), 0)
        self.assertEqual(len(msg.extra), 0)

        mbox.disconnect()

    def testConnectBadDaemon(self):
        silly = ' _._._42'
        try:
            spread.connect(silly)
        except spread.error:
            pass
        else:
            self.fail("expected spread.connect(%r) to complain" % silly)

        try:
            spread.connect(daemon=silly)
        except spread.error:
            pass
        else:
            self.fail("expected spread.connect(daemon=%r) to complain" % silly)

    def testConnectBadKeywords(self):
        for silly in 'demon', 'prior', 'member', 'xyz':
            try:
                spread.connect(**{silly: 1})
            except TypeError:
                pass
            else:
                self.fail("expected spread.connect to complain about bad "
                          "keyword argument %r" % silly)

    def testConnectDefaults(self):
        m = spread.connect(name='good')
        if not m.private_group.startswith('#good#'):
            self.fail(m.private_group)
        # The rest of the private_group name comes from whatever name was
        # given to the local daemon in the spread.conf file, and we don't
        # know what that is, so can't check it.
        m.disconnect()

    def testTwoConnect(self):
        group = self._group()

        mbox1 = self._connect()
        mbox1.join(group)

        mbox2 = self._connect()
        mbox2.join(group)

        msg1_1 = mbox1.receive()
        msg1_2 = mbox1.receive()
        msg2_1 = mbox2.receive()

        self.assertEqual(msg1_1.reason, spread.CAUSED_BY_JOIN)
        self.assertEqual(msg1_2.reason, spread.CAUSED_BY_JOIN)
        self.assertEqual(msg2_1.reason, spread.CAUSED_BY_JOIN)
        self.assertEqual(len(msg1_2.members), 2)
        self.assertEqual(msg1_2.members, msg2_1.members)

        mbox1.leave(group)
        mbox2.leave(group)
        mbox1.disconnect()
        mbox2.disconnect()

    def testTwoGroups(self):
        mbox = self._connect()
        group1 = self._group()
        group2 = self._group()

        mbox.join(group1)
        mbox.join(group2)
        mbox.leave(group1)
        mbox.leave(group2)

        msg1 = mbox.receive()
        msg2 = mbox.receive()
        msg3 = mbox.receive()
        msg4 = mbox.receive()

        self.assertEqual(msg1.reason, spread.CAUSED_BY_JOIN)
        self.assertEqual(msg2.reason, spread.CAUSED_BY_JOIN)
        self.assertEqual(len(msg1.members), 1)
        self.assertEqual(len(msg2.members), 1)
        self.assertEqual(msg1.members[0], mbox.private_group)
        self.assertEqual(msg3.reason, spread.CAUSED_BY_LEAVE)
        self.assertEqual(msg4.reason, spread.CAUSED_BY_LEAVE)
        self.assertEqual(len(msg3.members), 0)
        self.assertEqual(len(msg4.members), 0)

        mbox.disconnect()

    def testMultipleRecipients(self):
        group, members = self._connect_group(12)

        wr = members[0]
        wr.multicast(spread.FIFO_MESS, group, "1")

        for rd in members:
            msg = rd.receive()
            if not hasattr(msg, 'message'):
                print msg
                print msg.reason
                print msg.svc_type
            self.assertEqual(msg.message, "1")
            self.assertEqual(msg.sender, wr.private_group)
            self.assertEqual(len(msg.groups), 1)
            self.assertEqual(msg.groups[0], group)

        wr = members[0]
        wr.multicast(spread.FIFO_MESS, group, "2")

        for rd in members:
            msg = rd.receive()
            self.assertEqual(msg.message, "2")
            self.assertEqual(msg.sender, wr.private_group)
            self.assertEqual(len(msg.groups), 1)
            self.assertEqual(msg.groups[0], group)

    def testOneMessageMultipleRecipients(self):
        group, members = self._connect_group(12)

        member_private_names = tuple([m.private_group for m in members])

        wr = members[0]
        wr.multigroup_multicast(spread.FIFO_MESS, member_private_names, "1")

        for rd in members:
            msg = rd.receive()
            self.assert_(hasattr(msg, 'message'))
            self.assertEqual(msg.message, "1")
            self.assertEqual(msg.sender, wr.private_group)
            self.assertEqual(len(msg.groups), 12)
            self.failUnless(rd.private_group in msg.groups)

        wr.multigroup_multicast(spread.FIFO_MESS, member_private_names, "2")

        for rd in members:
            msg = rd.receive()
            self.assertEqual(msg.message, "2")
            self.assertEqual(msg.sender, wr.private_group)
            self.assertEqual(len(msg.groups), 12)
            self.failUnless(rd.private_group in msg.groups)

    def testBigMessage(self):
        group = self._group()
        mbox = self._connect()

        mbox.join(group)
        self.assertEqual(type(mbox.receive()), spread.MembershipMsgType)

        size = 2 * spread.DEFAULT_BUFFER_SIZE
        try:
            mbox.multicast(spread.SAFE_MESS, group, "X" * size)
        except spread.error, err:
            print err
            print size
            raise
        msg = mbox.receive()
        self.assertEqual(len(msg.message), size)

    def testBigGroup(self):
        group, members = self._connect_group(12)
        m1 = members[1]
        m2 = members[2]

        m2.leave(group)
        m1.receive()

    def testUseAfterClose(self):
        mbox = self._connect()
        mbox.disconnect()
        try:
            mbox.receive()
        except spread.error:
            pass
        else:
            self.fail("use after close should have raised spread.error")

    # Spawn a thread & send messages to it via Spread over the same mbox.
    # This will almost certainly deadlock if the receive call in the
    # receiver blocks all calls to Spread for the duration.
    def testSelfSend(self):
        try:
            import thread
        except ImportError:
            print "skipping testSelfSend() -- it requires threads"
            return
        mbox = self._connect(0)
        group = self._group()
        msgs = ['aaa', 'bbb', 'ccc', 'quit!']
        start = thread.allocate_lock()
        stop = thread.allocate_lock()
        start.acquire()
        stop.acquire()
        thread.start_new_thread(self.receiver,
                                (mbox, group, msgs, start, stop))
        for msg in msgs:
            start.acquire()
            mbox.multicast(spread.FIFO_MESS, group, msg)
        stop.acquire()

    # Helper for testSelfSend.
    def receiver(self, mbox, group, msgs, start, stop):
        mbox.join(group)
        for expected in msgs:
            start.release()
            msg = mbox.receive()
            self.assertEqual(msg.message, expected)
        mbox.leave(group)
        stop.release()

if __name__ == "__main__":
    unittest.main()
