/* Copyright (c) 2001-2005 Python Software Foundation.  All rights reserved.

   This code is released under the standard PSF license.
   See the file LICENSE.
*/

/* Python wrapper module for the Spread toolkit: http://www.spread.org/ */

#include "Python.h"
#include "structmember.h"
#include "sp.h"

#ifdef WITH_THREAD
/*
Jonathan Stanton (of Spread) verified multithreaded apps can suffer races
when Spread disconnects a mailbox, due to socket recycling (another thread,
which doesn't know about the disconnect, continues using the same socket
desciptor, but it can magically refer to a new connection).  They know
about this, and believe they know how to fix it, but haven't fixed it yet
(3.16.2).

We used to worm around it by setting the mbox disconnected flag to true when
Spread returns CONNECTION_CLOSED or ILLEGAL_SESSION.  Guido scoured the Spread
source and determined that those were the only Spread errors that closed
the socket descriptor.  Alas, it takes another level of locking to do the
{check that flag, call Spread, maybe set that flag} sequence indivisibly.

However that also has the effect of serializing all calls to Spread via a
given mailbox, and so a call to receive() in one thread blocks all threads
from invoking Spread on that mailbox until the receive() returns.  But if
the receive is waiting for a multicast from another thread, that's deadlock.

Until Spread disconnection semantics are fixed, we can't win:  we either
leave the Spread disconnection race unaddressed, or leave apps open to
deadlock.  For now we choose the former.
*/
#include "pythread.h"
/* #define SPREAD_DISCONNECT_RACE_BUG */
#endif

#ifdef SPREAD_DISCONNECT_RACE_BUG
/* Acquiring the lock is ugly, because another thread may be holding on to
   it:  we need to release the GIL in order to allow the other thread to
   make progress.
*/
#define ACQUIRE_MBOX_LOCK(MBOX)					\
	do {							\
		Py_BEGIN_ALLOW_THREADS				\
		PyThread_acquire_lock((MBOX)->spread_lock, 1);	\
		Py_END_ALLOW_THREADS				\
	} while(0)

#define RELEASE_MBOX_LOCK(MBOX) PyThread_release_lock((MBOX)->spread_lock)

#else
#define ACQUIRE_MBOX_LOCK(MBOX)
#define RELEASE_MBOX_LOCK(MBOX)
#endif

static PyObject *SpreadError;

#define DEFAULT_GROUPS_SIZE 10
#define DEFAULT_BUFFER_SIZE 10000

typedef struct {
	PyObject_HEAD
	mailbox mbox;
	PyObject *private_group;
	int disconnected;
#ifdef SPREAD_DISCONNECT_RACE_BUG
	PyThread_type_lock spread_lock;
#endif
} MailboxObject;

static PyObject *spread_error(int, MailboxObject *);

typedef struct {
	PyObject_HEAD
	PyObject *sender;
	PyObject *groups;
	int msg_type;
	int endian;
	PyObject *message;
} RegularMsg;

typedef struct {
	PyObject_HEAD
	int reason;
	PyObject *group;
	PyObject *group_id;
	PyObject *members;
	PyObject *extra; /* that are still members */
} MembershipMsg;

typedef struct {
	PyObject_HEAD
	group_id gid;
} GroupId;

staticforward PyTypeObject Mailbox_Type;
staticforward PyTypeObject RegularMsg_Type;
staticforward PyTypeObject MembershipMsg_Type;
staticforward PyTypeObject GroupId_Type;

#define MailboxObject_Check(v)	((v)->ob_type == &Mailbox_Type)
#define RegularMsg_Check(v)	((v)->ob_type == &RegularMsg_Type)
#define MembershipMsg_Check(v)	((v)->ob_type == &MembershipMsg_Type)
#define GroupId_Check(v)	((v)->ob_type == &GroupId_Type)

static PyObject *
new_group_id(group_id gid)
{
	GroupId *self;

	self = PyObject_New(GroupId, &GroupId_Type);
	if (!self)
		return NULL;
	self->gid = gid;
	return (PyObject *)self;
}

static void
group_id_dealloc(GroupId *v)
{
	PyObject_Del(v);
}

static PyObject *
group_id_repr(GroupId *v)
{
	char buf[80];
	sprintf(buf, "<group_id %08X:%08X:%08X>",
		v->gid.id[0], v->gid.id[1], v->gid.id[2]);
	return PyString_FromString(buf);
}

static PyObject *
group_id_richcompare(PyObject *v, PyObject *w, int op)
{
	PyObject *res;

	if (!GroupId_Check(v) || !GroupId_Check(w) ||
	    (op != Py_EQ && op != Py_NE)) {
		Py_INCREF(Py_NotImplemented);
		return Py_NotImplemented;
	}

	if (SP_equal_group_ids(((GroupId *)v)->gid, ((GroupId *)w)->gid) ==
	    (op == Py_NE))
		res = Py_False;
	else
		res = Py_True;
	Py_INCREF(res);
	return res;
}

static PyTypeObject GroupId_Type = {
	/* The ob_type field must be initialized in the module init function
	 * to be portable to Windows without using C++. */
	PyObject_HEAD_INIT(NULL)
	0,					/* ob_size */
	"GroupId",				/* tp_name */
	sizeof(GroupId),			/* tp_basicsize */
	0,					/* tp_itemsize */
	/* methods */
	(destructor)group_id_dealloc,		/* tp_dealloc */
	0,					/* tp_print */
	0,					/* tp_getattr */
	0,					/* tp_setattr */
	0,					/* tp_compare */
	(reprfunc)group_id_repr,		/* tp_repr */
	0,					/* tp_as_number */
	0,					/* tp_as_sequence */
	0,					/* tp_as_mapping */
	0,					/* tp_hash */
	0,					/* tp_call */
	0,					/* tp_str */
	0,					/* tp_getattro */
	0,					/* tp_setattro */
	0,					/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,			/* tp_flags */
	0,					/* tp_doc */
	0,					/* tp_traverse */
	0,					/* tp_clear */
	group_id_richcompare,			/* tp_richcompare */
};

#define CAUSED_BY_MASK (CAUSED_BY_JOIN | CAUSED_BY_LEAVE | \
                        CAUSED_BY_DISCONNECT | CAUSED_BY_NETWORK)

static PyObject *
new_membership_msg(int type, PyObject *group, int num_members,
		   char (*members)[MAX_GROUP_NAME], char *buffer, int size)
{
	MembershipMsg *self;
        int32 num_extra_members;
        group_id grp_id;
	/* Although extra_members isn't referenced unless num_extra_members is
	 * greater than 0, gcc doesn't realize that, so force extra_members to
	 * be initialized (suppressing a nuisance complaint from the compiler).
	 */
	char *extra_members = NULL;
	int i;

	assert(group != NULL);
	self = PyObject_New(MembershipMsg, &MembershipMsg_Type);
	if (self == NULL)
		return NULL;
	self->reason = type & CAUSED_BY_MASK; /* from sp.h defines */
	Py_INCREF(group);
	self->group = group;
	self->members = NULL;
	self->extra = NULL;
	self->group_id = NULL;
	self->members = PyTuple_New(num_members);
	if (self->members == NULL) {
		Py_DECREF(self);
		return NULL;
	}
	for (i = 0; i < num_members; ++i) {
		PyObject *s = PyString_FromString(members[i]);
		if (!s) {
			Py_DECREF(self);
			return NULL;
		}
		PyTuple_SET_ITEM(self->members, i, s);
	}

	num_extra_members = 0;
	if (SP_get_vs_set_offset_memb_mess() <= size) {
		/* Pick grp_id and num_extra_members out of the buffer.
		 * This uses memcpy instead of casting tricks because there's
		 * no guarantee that the offsets in buffer are properly
		 * aligned for the type.  Even gcc's memcpy() can produce
		 * segfaults then, unless the natural type is first cast
		 * away to char*.
		 */
		memcpy((char *)&grp_id,
		       buffer + SP_get_gid_offset_memb_mess(),
		       sizeof(grp_id));
                self->group_id = new_group_id(grp_id);
		if (self->group_id == NULL) {
			Py_DECREF(self);
			return NULL;
		}
		memcpy((char *)&num_extra_members,
		       buffer + SP_get_num_vs_offset_memb_mess(),
		       sizeof(num_extra_members));
                extra_members = buffer + SP_get_vs_set_offset_memb_mess();

		if (size - SP_get_vs_set_offset_memb_mess() <
		    num_extra_members * MAX_GROUP_NAME) {
			/* SP_receive error (corrupted message). */
			Py_DECREF(self);
			PyErr_Format(PyExc_AssertionError,
				"SP_receive:  a membership message said "
				"there were %d extra members, but only %d "
				"bytes remain in the buffer.  Corrupted "
				"message?",
			     	num_extra_members,
			     	size - SP_get_vs_set_offset_memb_mess());
			return NULL;
		}
	}

	self->extra = PyTuple_New(num_extra_members);
	if (self->extra == NULL) {
		Py_DECREF(self);
		return NULL;
	}
	for (i = 0; i < num_extra_members; i++, extra_members+= MAX_GROUP_NAME) {
		PyObject *s;
		/* Spread promises this: */
		assert(strlen(extra_members) < MAX_GROUP_NAME);
		s = PyString_FromString(extra_members);
		if (!s) {
			Py_DECREF(self);
			return NULL;
		}
		PyTuple_SET_ITEM(self->extra, i, s);
	}
	return (PyObject *)self;
}

static void
membership_msg_dealloc(MembershipMsg *self)
{
	Py_XDECREF(self->group);
	Py_XDECREF(self->members);
	Py_XDECREF(self->extra);
	Py_XDECREF(self->group_id);
	PyObject_Del(self);
}

#define OFF(x) offsetof(MembershipMsg, x)

static struct memberlist MembershipMsg_memberlist[] = {
	{"reason",	T_INT,		OFF(reason)},
	{"group",	T_OBJECT,	OFF(group)},
	{"group_id",	T_OBJECT,	OFF(group_id)},
	{"members",	T_OBJECT,	OFF(members)},
	{"extra",	T_OBJECT,	OFF(extra)},
	{NULL}
};

#undef OFF

static PyObject *
membership_msg_getattr(MembershipMsg *self, char *name)
{
	return PyMember_Get((char *)self, MembershipMsg_memberlist, name);
}

static PyTypeObject MembershipMsg_Type = {
	/* The ob_type field must be initialized in the module init function
	 * to be portable to Windows without using C++. */
	PyObject_HEAD_INIT(NULL)
	0,					/* ob_size */
	"MembershipMsg",			/* tp_name */
	sizeof(MembershipMsg),			/* tp_basicsize */
	0,					/* tp_itemsize */
	/* methods */
	(destructor)membership_msg_dealloc,	/* tp_dealloc */
	0,					/* tp_print */
	(getattrfunc)membership_msg_getattr,	/* tp_getattr */
	0,					/* tp_setattr */
	0,					/* tp_compare */
	0,					/* tp_repr */
	0,					/* tp_as_number */
	0,					/* tp_as_sequence */
	0,					/* tp_as_mapping */
};

static PyObject *
new_regular_msg(PyObject *sender, int num_groups,
		char (*groups)[MAX_GROUP_NAME], int msg_type,
		int endian, PyObject *message)
{
	RegularMsg *self;
	int i;

	self = PyObject_New(RegularMsg, &RegularMsg_Type);
	if (self == NULL)
		return NULL;

	self->message = NULL;
	self->sender = NULL;
	assert(num_groups >= 0);
	self->groups = PyTuple_New(num_groups);
	if (self->groups == NULL) {
		Py_DECREF(self);
		return NULL;
	}
	for (i = 0; i < num_groups; ++i) {
		PyObject *s = PyString_FromString(groups[i]);
		if (!s) {
			Py_DECREF(self);
			return NULL;
		}
		PyTuple_SET_ITEM(self->groups, i, s);
	}

	Py_INCREF(sender);
	self->sender = sender;
	Py_INCREF(message);
	self->message = message;
	self->msg_type = msg_type;
	self->endian = endian;
	return (PyObject *)self;
}

static void
regular_msg_dealloc(RegularMsg *self)
{
	Py_XDECREF(self->sender);
	Py_XDECREF(self->groups);
	Py_XDECREF(self->message);
	PyObject_Del(self);
}

#define OFF(x) offsetof(RegularMsg, x)

static struct memberlist RegularMsg_memberlist[] = {
	{"msg_type", T_INT,		OFF(msg_type)},
	{"endian",   T_INT,		OFF(endian)},
	{"sender",   T_OBJECT,		OFF(sender)},
	{"groups",   T_OBJECT,		OFF(groups)},
	{"message",  T_OBJECT,		OFF(message)},
	{NULL}
};

#undef OFF

static PyObject *
regular_msg_getattr(RegularMsg *self, char *name)
{
	return PyMember_Get((char *)self, RegularMsg_memberlist, name);
}

static PyTypeObject RegularMsg_Type = {
	/* The ob_type field must be initialized in the module init function
	 * to be portable to Windows without using C++. */
	PyObject_HEAD_INIT(NULL)
	0,					/* ob_size */
	"RegularMsg",				/* tp_name */
	sizeof(RegularMsg),			/* tp_basicsize */
	0,					/* tp_itemsize */
	/* methods */
	(destructor)regular_msg_dealloc,	/* tp_dealloc */
	0,					/* tp_print */
	(getattrfunc)regular_msg_getattr,	/* tp_getattr */
	0,					/* tp_setattr */
	0,					/* tp_compare */
	0,					/* tp_repr */
	0,					/* tp_as_number */
	0,					/* tp_as_sequence */
	0,					/* tp_as_mapping */
};

static MailboxObject *
new_mailbox(mailbox mbox)
{
	MailboxObject *self;
	self = PyObject_New(MailboxObject, &Mailbox_Type);
	if (self == NULL)
		return NULL;
	self->mbox = mbox;
	self->private_group = NULL;
	self->disconnected = 0;
#ifdef SPREAD_DISCONNECT_RACE_BUG
	self->spread_lock = NULL;
#endif
	return self;
}

/* mailbox methods */

static void
mailbox_dealloc(MailboxObject *self)
{
	if (self->disconnected == 0)
		SP_disconnect(self->mbox);
	Py_DECREF(self->private_group);
#ifdef SPREAD_DISCONNECT_RACE_BUG
	if (self->spread_lock)
		PyThread_free_lock(self->spread_lock);
#endif
	PyObject_Del(self);
}

static PyObject *
err_disconnected(char *methodname)
{
	PyErr_Format(SpreadError, "%s() called on closed mbox", methodname);
	return NULL;
}

static PyObject *
mailbox_disconnect(MailboxObject *self, PyObject *args)
{
	PyObject *result = Py_None;

	if (!PyArg_ParseTuple(args, ":disconnect"))
		return NULL;
	if (!self->disconnected) {
		ACQUIRE_MBOX_LOCK(self);
		if (!self->disconnected) {
			int err;
			self->disconnected = 1;
			Py_BEGIN_ALLOW_THREADS
			err = SP_disconnect(self->mbox);
			Py_END_ALLOW_THREADS
			if (err != 0)
				result = spread_error(err, self);
		}
		RELEASE_MBOX_LOCK(self);
	}
	Py_XINCREF(result);
	return result;
}

static PyObject *
mailbox_fileno(MailboxObject *self, PyObject *args)
{
	if (!PyArg_ParseTuple(args, ":fileno"))
		return NULL;
	if (self->disconnected)
		return err_disconnected("fileno");
	return PyInt_FromLong(self->mbox);
}

static PyObject *
mailbox_join(MailboxObject *self, PyObject *args)
{
	char *group;
	PyObject *result = Py_None;

	if (!PyArg_ParseTuple(args, "s:join", &group))
		return NULL;
	ACQUIRE_MBOX_LOCK(self);
	if (self->disconnected)
		result = err_disconnected("join");
	else {
		int err;
		Py_BEGIN_ALLOW_THREADS
		err = SP_join(self->mbox, group);
		Py_END_ALLOW_THREADS
		if (err < 0)
			result = spread_error(err, self);
	}
	RELEASE_MBOX_LOCK(self);
	Py_XINCREF(result);
	return result;
}

static PyObject *
mailbox_leave(MailboxObject *self, PyObject *args)
{
	char *group;
	PyObject *result = Py_None;

	if (!PyArg_ParseTuple(args, "s:leave", &group))
		return NULL;
	ACQUIRE_MBOX_LOCK(self);
	if (self->disconnected)
		result = err_disconnected("leave");
	else {
		int err;
		Py_BEGIN_ALLOW_THREADS
		err = SP_leave(self->mbox, group);
		Py_END_ALLOW_THREADS
		if (err < 0)
			result = spread_error(err, self);
	}
	RELEASE_MBOX_LOCK(self);
	Py_XINCREF(result);
	return result;
}

static PyObject *
mailbox_receive(MailboxObject *self, PyObject *args)
{
	/* CAUTION:  initializing svc_type is critical.  It's not clear from
	 * the docs, but this is an input as well as an output parameter.
	 * We didn't initialize it before, and very rarely the DROP_RECV flag
	 * would end up getting set in it.  That in turn has miserable
	 * consequences, and consequences only visible if a buffer (data or
	 * group) is too small for the msg being received (so it goes crazy
	 * at the worst possible times).
	 */
	service svc_type;
	int num_groups, endian, size;
	int16 msg_type;

	char senderbuffer[MAX_GROUP_NAME];
	char groupbuffer[DEFAULT_GROUPS_SIZE][MAX_GROUP_NAME];
	char databuffer[DEFAULT_BUFFER_SIZE];

	int max_groups = DEFAULT_GROUPS_SIZE;
	char (*groups)[MAX_GROUP_NAME] = groupbuffer;

	int bufsize = DEFAULT_BUFFER_SIZE;
	char *pbuffer = databuffer;

	PyObject *sender = NULL, *data = NULL, *msg = NULL;

	if (!PyArg_ParseTuple(args, ":receive"))
		return NULL;

	ACQUIRE_MBOX_LOCK(self);
	if (self->disconnected) {
		err_disconnected("receive");
		goto error;
	}

	for (;;) {
		char *assertmsg = "internal error";

		Py_BEGIN_ALLOW_THREADS
		svc_type = 0;	/* initializing this is critical */
		size = SP_receive(self->mbox, &svc_type,
				  senderbuffer,
				  max_groups, &num_groups, groups,
				  &msg_type, &endian,
				  bufsize, pbuffer);
		Py_END_ALLOW_THREADS

		if (size >= 0) {
			if (num_groups < 0) {
				/* This isn't possible unless DROP_RECV is
				 * passed to SP_receive in svc_type.
				 */
				assertmsg = "size >= 0 and num_groups < 0";
				goto assert_error;
			}
			if (endian < 0) {
				/* This should never be possible. */
				assertmsg = "size >= 0 and endian < 0";
				goto assert_error;
			}
			break;	/* This is the only normal loop exit. */
		}
		if (size == BUFFER_TOO_SHORT) {
			if (endian >= 0) {
				/* This isn't possible unless DROP_RECV is
				 * passed to SP_receive in svc_type.
				 */
				assertmsg = "BUFFER_TOO_SHORT and endian >= 0";
				goto assert_error;
			}
			bufsize = - endian;
			Py_XDECREF(data);
			data = PyString_FromStringAndSize(NULL, bufsize);
			if (data == NULL)
				goto error;
			pbuffer = PyString_AS_STRING(data);
			continue;
		}
		if (size == GROUPS_TOO_SHORT) {
			/* If the data buffer and the group buffer are both
			 * too small, and DROP_RECV was not specified, then
			 * Jonathan Stanton said GROUPS_TOO_SHORT is returned.
			 * If both are too short and DROP_RECV is specified,
			 * then BUFFER_TOO_SHORT is returned.  "Backward
			 * compatibility" headaches.  For simplicity, we only
			 * deal with one "too short" condition per loop trip.
			 * When we loop back, SP_receive should tell us
			 * about the other (if another thread hasn't already
			 * grabbed the msg).
			 */
			if (num_groups >= 0) {
				/* This shouldn never be possible. */
				assertmsg = "GROUPS_TOO_SHORT and num_groups >= 0";
				goto assert_error;
			}
			max_groups = - num_groups;
			if (groups != groupbuffer)
				free(groups);
			groups = malloc(MAX_GROUP_NAME * max_groups);
			if (groups == NULL) {
				PyErr_NoMemory();
				goto error;
			}
			continue;
		}
		/* There's a real error we can't deal with (e.g., Spread
		 * got disconnected).
		 */
		spread_error(size, self);
		goto error;
assert_error:
		PyErr_Format(PyExc_AssertionError,
			     "SP_receive: %s; "
			     "size=%d svc_type=%d num_groups=%d "
			     "msg_type=%d endian=%d",
			     assertmsg,
			     size, svc_type, num_groups, msg_type, endian);
		goto error;
	}

	/* It's not clear from the SP_receive() man page what all the
	   possible categories of services types are possible. */

	sender = PyString_FromString(senderbuffer);
	if (sender == NULL)
		goto error;

	if (Is_regular_mess(svc_type)) {
		if (data == NULL) {
			data = PyString_FromStringAndSize(databuffer, size);
			if (data == NULL)
				goto error;
		}
		else if (PyString_GET_SIZE(data) != size) {
			if (_PyString_Resize(&data, size) < 0)
				goto error;
		}
		msg = new_regular_msg(sender, num_groups, groups,
				      msg_type, endian, data);
	}
	else if (Is_membership_mess(svc_type)) {
		/* XXX Mark transitional messages */
		msg = new_membership_msg(svc_type, sender,
					 num_groups, groups,
					 pbuffer, size);
	}
	else {
		PyErr_Format(SpreadError,
			     "unexpected service type: 0x%x", svc_type);
		goto error;
	}

  error:
	RELEASE_MBOX_LOCK(self);
	if (groups != groupbuffer)
		free(groups);
	Py_XDECREF(sender);
	Py_XDECREF(data);
	return msg;
}

const int valid_svc_type = (UNRELIABLE_MESS | RELIABLE_MESS | FIFO_MESS
			    | CAUSAL_MESS | AGREED_MESS | SAFE_MESS
			    | SELF_DISCARD);

static PyObject *
mailbox_multicast(MailboxObject *self, PyObject *args)
{
	int svc_type, bytes, msg_len;
	int msg_type = 0;
	char *group, *msg;
	PyObject *result = NULL;

	if (!PyArg_ParseTuple(args, "iss#|i:multicast",
			      &svc_type, &group, &msg, &msg_len, &msg_type))
		return NULL;

	ACQUIRE_MBOX_LOCK(self);
	if (self->disconnected) {
		err_disconnected("multicast");
		goto Done;
	}
	/* XXX This doesn't check that svc_type is set to exactly one of
	   the service types. */
	if ((svc_type & valid_svc_type) != svc_type) {
		PyErr_SetString(PyExc_ValueError, "invalid service type");
		goto Done;;
	}

	Py_BEGIN_ALLOW_THREADS
	bytes = SP_multicast(self->mbox, svc_type, group, (int16)msg_type,
			     msg_len, msg);
	Py_END_ALLOW_THREADS
	if (bytes < 0)
		result = spread_error(bytes, self);
	else
		result = PyInt_FromLong(bytes);
Done:
	RELEASE_MBOX_LOCK(self);
	return result;
}

static PyObject *
mailbox_multigroup_multicast(MailboxObject *self, PyObject *args)
{
        int svc_type, bytes, msg_len, group_len;
        int msg_type = 0;
	PyObject *group_tuple, *temp;
        char *msg;

	char (*groups)[MAX_GROUP_NAME];
	int index;

        PyObject *result = NULL;

        if (! PyArg_ParseTuple(args, "iO!s#|i:multicast",
                               &svc_type,
                               &PyTuple_Type, &group_tuple,
                               &msg, &msg_len,
                               &msg_type))
                return NULL;

	if(! PyTuple_Check(group_tuple)) {
		PyErr_SetString(PyExc_TypeError,
				"only tuples are allowed for groups");
		return NULL;
	}

	group_len = PyTuple_Size(group_tuple);
	if (group_len == 0) {
		PyErr_SetString(PyExc_ValueError,
				"there must be at least one group in the tuple");
		return NULL;
	}

	groups = malloc(MAX_GROUP_NAME * group_len);
	if (groups == NULL) {
		PyErr_NoMemory();
		return NULL;
	}

	for (index = 0; index < group_len; index++) {
		temp = PyTuple_GetItem(group_tuple, index);
		if(! PyString_Check(temp)) {
			PyErr_SetString(PyExc_TypeError,
					"groups must be strings only");
			goto Done;
		}
		strncpy(groups[index],
			PyString_AsString(PyTuple_GetItem(group_tuple, index)),
			MAX_GROUP_NAME);
	}

	ACQUIRE_MBOX_LOCK(self);
	if (self->disconnected) {
		err_disconnected("multigroup_multicast");
		goto Done;
	}

	/* XXX This doesn't check that svc_type is set to exactly one of
	   the service types. */
	if ((svc_type & valid_svc_type) != svc_type) {
		PyErr_SetString(PyExc_ValueError, "invalid service type");
		goto Done;
	}

	Py_BEGIN_ALLOW_THREADS
	bytes = SP_multigroup_multicast(self->mbox, svc_type, group_len,
				        (const char (*)[MAX_GROUP_NAME]) groups,
		                        (int16)msg_type, msg_len, msg);
	Py_END_ALLOW_THREADS

	if (bytes < 0)
		result = spread_error(bytes, self);
	else
		result = PyInt_FromLong(bytes);

Done:
	RELEASE_MBOX_LOCK(self);
	free(groups);
	return result;
}

static PyObject *
mailbox_poll(MailboxObject *self, PyObject *args)
{
	int bytes;
	PyObject *result = NULL;

	if (!PyArg_ParseTuple(args, ":poll"))
		return NULL;
	ACQUIRE_MBOX_LOCK(self);
	if (self->disconnected) {
		err_disconnected("poll");
		goto Done;
	}
	Py_BEGIN_ALLOW_THREADS
	bytes = SP_poll(self->mbox);
	Py_END_ALLOW_THREADS
	if (bytes < 0)
		result = spread_error(bytes, self);
	else
		result = PyInt_FromLong(bytes);
Done:
	RELEASE_MBOX_LOCK(self);
	return result;
}

static PyMethodDef Mailbox_methods[] = {
	{"disconnect",	(PyCFunction)mailbox_disconnect,METH_VARARGS},
	{"fileno",	(PyCFunction)mailbox_fileno,	METH_VARARGS},
	{"join",	(PyCFunction)mailbox_join,	METH_VARARGS},
	{"leave",	(PyCFunction)mailbox_leave,	METH_VARARGS},
	{"multicast",   (PyCFunction)mailbox_multicast, METH_VARARGS},
	{"multigroup_multicast",	(PyCFunction)mailbox_multigroup_multicast, METH_VARARGS},
	{"poll",	(PyCFunction)mailbox_poll,	METH_VARARGS},
	{"receive",	(PyCFunction)mailbox_receive,	METH_VARARGS},
	{NULL,		NULL}		/* sentinel */
};

#define OFF(x) offsetof(MailboxObject, x)

static struct memberlist Mailbox_memberlist[] = {
	{"private_group",	T_OBJECT,	OFF(private_group)},
	{NULL}
};

static PyObject *
mailbox_getattr(PyObject *self, char *name)
{
	PyObject *meth;

	meth = Py_FindMethod(Mailbox_methods, self, name);
	if (meth)
		return meth;
	PyErr_Clear();
	return PyMember_Get((char *)self, Mailbox_memberlist, name);
}

static PyTypeObject Mailbox_Type = {
	/* The ob_type field must be initialized in the module init function
	 * to be portable to Windows without using C++. */
	PyObject_HEAD_INIT(NULL)
	0,					/* ob_size */
	"Mailbox",				/* tp_name */
	sizeof(MailboxObject),			/* tp_basicsize */
	0,					/* tp_itemsize */
	/* methods */
	(destructor)mailbox_dealloc,		/* tp_dealloc */
	0,					/* tp_print */
	(getattrfunc)mailbox_getattr,		/* tp_getattr */
	0,					/* tp_setattr */
	0,					/* tp_compare */
	0,					/* tp_repr */
	0,					/* tp_as_number */
	0,					/* tp_as_sequence */
	0,					/* tp_as_mapping */
};

static char spread_connect__doc__[] =
"connect(daemon=\"N@localhost\", name=\"\", priority=0, membership=1) -> mbox\n"
"\n"
"All arguments are optional, and can be specified by keyword or position.\n"
"\n"
"Connect to a Spread daemon, via Spread's SP_connect().  Return a Mailbox\n"
"object representing the connection.  Communication with Spread on this\n"
"connection is done via invoking methods of the Mailbox object.\n"
"\n"
"'daemon' is the name of the desired Spread daemon.  It defaults to\n"
"    \"%d@localhost\" % spread.DEFAULT_SPREAD_PORT\n"
"'name' is the desired private name for the connection.  It defaults to an\n"
"    empty string, in which case Spread generates a unique random name.\n"
"'priority' is an int, default 0, currently unused (see Spread docs).\n"
"'membership' is a Boolean, default 1 (true), determining whether you want\n"
"    to receive membership messages on this connection.  If your application\n"
"    doesn't make mbox.receive() calls, pass 0 to avoid creating an\n"
"    unboundedly large queue of unread membership messages.\n"
"\n"
"Upon successful connect, mbox.private_group is the private group name\n"
"Spread assigned to the connection.";

static PyObject *
spread_connect(PyObject *self, PyObject *args, PyObject* kwds)
{
	static char *kwlist[] = {"daemon", "name", "priority", "membership",
				  0};
	char *daemon = NULL;
	char *name = "";
	int priority = 0;
	int membership = 1;

	MailboxObject *mbox;
	mailbox _mbox;
	int ret;
	PyObject *group_name = NULL;
	char default_daemon[100];

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "|ssii:connect", kwlist,
					 &daemon, &name, &priority,
					 &membership))
		return NULL;

	if (daemon == NULL) {
		/* XXX Can't use PyOS_snprintf before 2.2.
		PyOS_snprintf(default_daemon, sizeof(default_daemon),
			      "%d@localhost", DEFAULT_SPREAD_PORT);
		*/
		sprintf(default_daemon, "%d@localhost", DEFAULT_SPREAD_PORT);
		daemon = default_daemon;
	}

	/* initialize output buffer for group name */
	group_name = PyString_FromStringAndSize(NULL, MAX_GROUP_NAME);
	if (group_name == NULL)
		return NULL;

	Py_BEGIN_ALLOW_THREADS
	ret = SP_connect(daemon, name, priority, membership, &_mbox,
			 PyString_AS_STRING(group_name));
	Py_END_ALLOW_THREADS

	if (ret != ACCEPT_SESSION) {
		Py_DECREF(group_name);
		return spread_error(ret, NULL);
	}

	mbox = new_mailbox(_mbox);
	if (mbox == NULL) {
		SP_disconnect(_mbox);
		Py_DECREF(group_name);
		return NULL;
	}
#ifdef SPREAD_DISCONNECT_RACE_BUG
	mbox->spread_lock = PyThread_allocate_lock();
	if (mbox->spread_lock == NULL) {
		Py_DECREF(mbox);
		return NULL;
	}
#endif
	if (_PyString_Resize(
		&group_name,
		strlen(PyString_AS_STRING(group_name))) < 0)
	{
		SP_disconnect(_mbox);
		Py_DECREF(mbox);
		return NULL;
	}
	mbox->private_group = group_name;
	return (PyObject*)mbox;
}

static char spread_version__doc__[] =
"version() -> (major, minor, patch)\n"
"\n"
"Return Spread's version number as a 3-tuple of integers, as obtained\n"
"from Spread's SP_version().";

static PyObject *
spread_version(PyObject *self, PyObject *args)
{
	int major, minor, patch;

	if (!PyArg_ParseTuple(args, ":version"))
		return NULL;
	if (!SP_version(&major, &minor, &patch)) {
		PyErr_SetString(SpreadError, "SP_version failed");
		return NULL;
	}
	return Py_BuildValue("iii", major, minor, patch);
}

/* List of functions defined in the module */

static PyMethodDef spread_methods[] = {
	{"connect", (PyCFunction)spread_connect, METH_VARARGS | METH_KEYWORDS,
	 spread_connect__doc__},
	{"version", spread_version, METH_VARARGS,
	 spread_version__doc__},
	{NULL, NULL}		/* sentinel */
};

/* spread_error(): helper function for setting exceptions from SP_xxx
   return value */

static PyObject *
spread_error(int err, MailboxObject *mbox)
{
	char *message = NULL;
	PyObject *val;

	/* XXX It would be better if spread provided an API function to
	   map these to error strings.  SP_error() merely prints a string,
	   which is useful in only limited circumstances. */
	switch (err) {
	case ILLEGAL_SPREAD:
		message = "Illegal spread was provided";
		break;
	case COULD_NOT_CONNECT:
		message = "Could not connect. Is Spread running?";
		break;
	case REJECT_QUOTA:
		message = "Connection rejected, too many users";
		break;
	case REJECT_NO_NAME:
		message = "Connection rejected, no name was supplied";
		break;
	case REJECT_ILLEGAL_NAME:
		message = "Connection rejected, illegal name";
		break;
	case REJECT_NOT_UNIQUE:
		message = "Connection rejected, name not unique";
		break;
	case REJECT_VERSION:
		message = "Connection rejected, library does not fit daemon";
		break;
	case CONNECTION_CLOSED:
		message = "Connection closed by spread";
		if (mbox)
			mbox->disconnected = 1;
		break;
	case REJECT_AUTH:
		message = "Connection rejected, authentication failed";
		break;
	case ILLEGAL_SESSION:
		message = "Illegal session was supplied";
		if (mbox)
			mbox->disconnected = 1;
		break;
	case ILLEGAL_SERVICE:
		message = "Illegal service request";
		break;
	case ILLEGAL_MESSAGE:
		message = "Illegal message";
		break;
	case ILLEGAL_GROUP:
		message = "Illegal group";
		break;
	case BUFFER_TOO_SHORT:
		message = "The supplied buffer was too short";
		break;
	case GROUPS_TOO_SHORT:
		message = "The supplied groups list was too short";
		break;
	case MESSAGE_TOO_LONG:
		message = "The message body + group names "
			  "was too large to fit in a message";
		break;
	default:
		message = "unrecognized error";
	}

	val = Py_BuildValue("is", err, message);
	if (val) {
		PyErr_SetObject(SpreadError, val);
		Py_DECREF(val);
	}

	return NULL;
}

/* Table of symbolic constants defined by Spread.
   (Programmatically generated from sp.h.) */

static struct constdef {
	char *name;
	int value;
} spread_constants[] = {
	{"LOW_PRIORITY", LOW_PRIORITY},
	{"MEDIUM_PRIORITY", MEDIUM_PRIORITY},
	{"HIGH_PRIORITY", HIGH_PRIORITY},
	{"DEFAULT_SPREAD_PORT", DEFAULT_SPREAD_PORT},
	{"SPREAD_VERSION", SPREAD_VERSION},
	{"MAX_GROUP_NAME", MAX_GROUP_NAME},
	{"MAX_PRIVATE_NAME", MAX_PRIVATE_NAME},
	{"MAX_PROC_NAME", MAX_PROC_NAME},
	{"UNRELIABLE_MESS", UNRELIABLE_MESS},
	{"RELIABLE_MESS", RELIABLE_MESS},
	{"FIFO_MESS", FIFO_MESS},
	{"CAUSAL_MESS", CAUSAL_MESS},
	{"AGREED_MESS", AGREED_MESS},
	{"SAFE_MESS", SAFE_MESS},
	{"REGULAR_MESS", REGULAR_MESS},
	{"SELF_DISCARD", SELF_DISCARD},
	{"DROP_RECV", DROP_RECV},
	{"REG_MEMB_MESS", REG_MEMB_MESS},
	{"TRANSITION_MESS", TRANSITION_MESS},
	{"CAUSED_BY_JOIN", CAUSED_BY_JOIN},
	{"CAUSED_BY_LEAVE", CAUSED_BY_LEAVE},
	{"CAUSED_BY_DISCONNECT", CAUSED_BY_DISCONNECT},
	{"CAUSED_BY_NETWORK", CAUSED_BY_NETWORK},
	{"MEMBERSHIP_MESS", MEMBERSHIP_MESS},
	{"ENDIAN_RESERVED", ENDIAN_RESERVED},
	{"RESERVED", RESERVED},
	{"REJECT_MESS", REJECT_MESS},
	{"ACCEPT_SESSION", ACCEPT_SESSION},
	{"ILLEGAL_SPREAD", ILLEGAL_SPREAD},
	{"COULD_NOT_CONNECT", COULD_NOT_CONNECT},
	{"REJECT_QUOTA", REJECT_QUOTA},
	{"REJECT_NO_NAME", REJECT_NO_NAME},
	{"REJECT_ILLEGAL_NAME", REJECT_ILLEGAL_NAME},
	{"REJECT_NOT_UNIQUE", REJECT_NOT_UNIQUE},
	{"REJECT_VERSION", REJECT_VERSION},
	{"CONNECTION_CLOSED", CONNECTION_CLOSED},
	{"REJECT_AUTH", REJECT_AUTH},
	{"ILLEGAL_SESSION", ILLEGAL_SESSION},
	{"ILLEGAL_SERVICE", ILLEGAL_SERVICE},
	{"ILLEGAL_MESSAGE", ILLEGAL_MESSAGE},
	{"ILLEGAL_GROUP", ILLEGAL_GROUP},
	{"BUFFER_TOO_SHORT", BUFFER_TOO_SHORT},
	{"GROUPS_TOO_SHORT", GROUPS_TOO_SHORT},
	{"MESSAGE_TOO_LONG", MESSAGE_TOO_LONG},

	/* Not Spread constants, but still useful */
	{"DEFAULT_BUFFER_SIZE", DEFAULT_BUFFER_SIZE},
	{"DEFAULT_GROUPS_SIZE", DEFAULT_GROUPS_SIZE},
	{NULL}
};

/* Initialization function for the module */

DL_EXPORT(void)
initspread(void)
{
	PyObject *m;
	struct constdef *p;

	/* Create the module and add the functions */
	m = Py_InitModule("spread", spread_methods);
	if (m == NULL)
		return;

	/* Initialize the type of the new type object here; doing it here
	 * is required for portability to Windows without requiring C++. */
	Mailbox_Type.ob_type = &PyType_Type;
	RegularMsg_Type.ob_type = &PyType_Type;
	MembershipMsg_Type.ob_type = &PyType_Type;

	/* PyModule_AddObject() DECREFs its third argument */
	Py_INCREF(&Mailbox_Type);
	if (PyModule_AddObject(m, "MailboxType",
			       (PyObject *)&Mailbox_Type) < 0)
		return;
	Py_INCREF(&RegularMsg_Type);
	if (PyModule_AddObject(m, "RegularMsgType",
			       (PyObject *)&RegularMsg_Type) < 0)
		return;
	Py_INCREF(&MembershipMsg_Type);
	if (PyModule_AddObject(m, "MembershipMsgType",
			       (PyObject *)&MembershipMsg_Type) < 0)
		return;

	/* Create the exception, if necessary */
	if (SpreadError == NULL) {
		SpreadError = PyErr_NewException("spread.error", NULL, NULL);
		if (SpreadError == NULL)
			return;
	}

	/* Add the exception to the module */
	Py_INCREF(SpreadError);
	if (PyModule_AddObject(m, "error", SpreadError) < 0)
		return;

	/* Add the Spread symbolic constants to the module */
	for (p = spread_constants; p->name != NULL; p++) {
		if (PyModule_AddIntConstant(m, p->name, p->value) < 0)
			return;
	}

}
