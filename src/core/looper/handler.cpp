#include "stdafx.h"
#include "looper/handler.h"
#include "looper/looper.h"
#include "string/textseparator.h"
#include "timermanager.h"
#include "timernode.h"
#include "asynctask.h"
#include "base/stringtool.h"
#include "timerextrainfo.h"
#include "message.inl"
#include "handlerinternaldata.h"

#ifdef _MSC_VER_DEBUG
#define new DEBUG_NEW
#endif

#ifdef _CONFIG_ANDROID
extern "C"
{
#include <android/log.h>
}
#endif

using namespace std;

namespace Bear {
namespace Core
{
using namespace FileSystem;

struct tagFindObjectInfo
{
	shared_ptr<Handler> mHandler;
};

struct tagGetShortcut
{
	shared_ptr<Handler> mHandler;
};

struct tagCancelRunnable
{
	shared_ptr<Handler>				mHandler;
	shared_ptr<Runnable>			mRunnable;
};

Handler::Handler()
{
	mInternalData = make_shared<tagHandlerInternalData>(this);
#ifdef _DEBUG
	SetObjectName("Handler");
#endif

	//在没有调用Create绑定到looper之前，认为Handler是自由的,threadId为0
	mThreadId = 0;// looper ? looper->GetThreadId() : ShellTool::GetCurrentThreadId();

#ifdef _CONFIG_MONITOR_HANDLER
	{
		AutoLock lock(&tagHandlerInternalData::gCSBaseHandler);
		tagHandlerInternalData::gHandlers[(long*)this] = (long*)this;

		int nc = (int)tagHandlerInternalData::gHandlers.size();
		if (tagHandlerInternalData::gRationalHandlerUpperLimit && nc > tagHandlerInternalData::gRationalHandlerUpperLimit)
		{
			DW("new Handler(0x%p) out of limit,count=%d,limit=%d", this, nc, tagHandlerInternalData::gRationalHandlerUpperLimit);
		}
	}
#endif
}

Handler::~Handler()
{
	mInternalData->mHandler = nullptr;

	if (mInternalData->mTickDestroy)
	{
		auto tick = ShellTool::GetTickCount64() - mInternalData->mTickDestroy;
		if (tick > 50)
		{
			DV("%s(%s),this=%p,destroy tick=%lld", __func__, GetObjectName().c_str(), this, tick);
		}
	}

#ifdef _CONFIG_MONITOR_HANDLER
	{
		AutoLock lock(&tagHandlerInternalData::gCSBaseHandler);
		auto iter = tagHandlerInternalData::gHandlers.find((long*)this);
		if (iter != tagHandlerInternalData::gHandlers.end())
		{
			tagHandlerInternalData::gHandlers.erase(iter);
		}
	}
#endif
}

LRESULT Handler::sendMessage(UINT msg, WPARAM wp, LPARAM lp)
{
	if (!mInternalData->mCreated && msg!=BM_CREATE)
	{
		DW("fail sendMessage,need call Create()");
		return 0;
	}

	if (!mInternalData->mLooper)
	{
		DW("mInternalData->mLooper is null,msg=%d,name=%s", msg,GetObjectName().c_str());
		return 0;
	}

	return mInternalData->mLooper->sendMessage(shared_from_this(),msg,wp,lp);
}

LRESULT Handler::sendMessage(shared_ptr<Message> obj)
{
	if (!obj)
	{
		return 0;
	}

	return sendMessage(BM_MESSAGE, (WPARAM)&obj);
}

LRESULT Handler::postMessage(shared_ptr<Message> obj)
{
	if (!IsCreated() || !obj)
	{
		DW("fail %s,IsCreated()=%d", __func__, IsCreated());
		return 0;
	}

	auto info = make_shared<tagSelfRef>();
	info->mSelfRef = info;
	info->mObject = obj;
	auto ret = postMessage(BM_POST_MESSAGE, (WPARAM)info.get());
	if (ret)
	{
		info->mSelfRef = nullptr;
	}
	return ret;
}

LRESULT Handler::sendRunnable(shared_ptr<Runnable> obj)
{
	if (!obj)
	{
		return 0;
	}

	auto info = make_shared<tagDelayedRunnable>();
	info->mHandler = shared_from_this();
	info->mRunnable = obj;
	info->mSelfRef = info;
	return sendMessage(BM_POST_RUNNABLE, (WPARAM)info.get());
}

LRESULT Handler::postMessage(UINT msg, WPARAM wp, LPARAM lp)
{
	if (!mInternalData->mCreated)
	{
		DW("fail sendMessage,need call Create()");
		return 0;
	}

	if (!mInternalData->mLooper)
	{
		DW("mInternalData->mLooper is null");
		return 0;
	}

	return mInternalData->mLooper->postMessage(shared_from_this(), msg, wp, lp);
}

//由所属looper调用
LRESULT Handler::OnMessage(UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg < BM_NULL)
	{
		auto iter = mInternalData->mMessageEntries.find(msg);
		if (iter != mInternalData->mMessageEntries.end())
		{
			return (this->*(iter->second))(wp, lp);
		}

		return 0;
	}

	switch ((unsigned long)msg)
	{
	case BM_NULL:
	{
		return 0;
	}
	case BM_CREATE:
	{
		if (IsCreated())
		{
			DV("skip double BM_CREATE");
		}
		else
		{
			mInternalData->mCreated = true;
			OnCreate();
			if (!mInternalData->mOnCreateCalled)
			{
				DW("%s,Handler::OnCreate() is NOT called,make sure __super::OnCreate() is called.", GetObjectName().c_str());
				ASSERT(FALSE);
			}
		}

		return 0;
	}
	case BM_DESTROY:
	{
		if (!mInternalData->mDestroyMarkCalled)
		{
			mInternalData->mDestroyMarkCalled = true;
			OnDestroy();

			//当用户忘记调用基类OnDestroy()时给出提醒
			if (!mInternalData->mOnDestroyCalled)
			{
				DW("%s,Handler::OnDestroy() is NOT called,make sure __super::OnDestroy() is called.",GetObjectName().c_str());
				ASSERT(FALSE);
			}
		}
		return 0;
	}
	case BM_DUMP:
	{
		mInternalData->Dump((int)(long)wp, (bool)lp);
		return 0;
	}
	case BM_DUMP_ALL:
	{
		mInternalData->DumpAll();
		return 0;
	}
	case BM_ADD_CHILD:
	{
		weak_ptr<Handler> *child = (weak_ptr<Handler>*)wp;
		const char *name = (const char*)lp;
		return mInternalData->AddChildHelper(*child, name);
	}
	case BM_FIND_OBJECT:
	{
		tagFindObjectInfo &info = *(tagFindObjectInfo*)wp;
		string& url = *(string*)lp;
		info.mHandler = mInternalData->FindObject_Impl(url);
		return 0;
	}
	case BM_GET_CHILD:
	{
		tagFindObjectInfo &info = *(tagFindObjectInfo*)wp;
		LONG_PTR id = (LONG_PTR)lp;
		info.mHandler = mInternalData->GetChild_Impl(id);
		return 0;
	}
	case BM_GET_SHORTCUT:
	{
		tagGetShortcut &info = *(tagGetShortcut*)wp;
		const string& url = *(string*)lp;
		info.mHandler = mInternalData->Shortcut_Impl(url);
		return 0;
	}
	case BM_REGISTER_SHORTCUT:
	{
		auto name = (string*)wp;
		auto obj = (weak_ptr<Handler>*)lp;
		return mInternalData->RegisterShortcut_Impl(*name, *obj);
	}
	
	case BM_PRE_EXECUTE:
	{
		auto obj = dynamic_pointer_cast<AsyncTask>(shared_from_this());
		if (obj)
		{
			obj->OnPreExecute();
		}
		return 0;
	}
	case BM_POST_EXECUTE:
	{
		auto obj = dynamic_pointer_cast<AsyncTask>(shared_from_this());
		if (obj)
		{
			obj->OnPostExecute();
		}
		return 0;
	}
	case BM_REMOVE_CHILD_WEAKREF:
	{
		Handler *handler = (Handler *)wp;
		mInternalData->RemoveChildWeakRef_Impl(handler);
		return 0;
	}
	case BM_POST_RUNNABLE:
	{
		auto info = (tagDelayedRunnable*)wp;
		if (info->mDelayedMS == 0)
		{
			info->mRunnable->Run();
		}
		else
		{
			auto infoEx = make_shared<tagTimerExtraInfo>();
			infoEx->mRunnable = info->mRunnable;
			mInternalData->SetTimerEx(info->mDelayedMS, infoEx);
		}
		info->mSelfRef = nullptr;
		return 0;
	}
	case BM_CANCEL_RUNNABLE:
	{
		auto info = (tagCancelRunnable*)wp;
		{
			Looper::CurrentLooper()->CancelRunnableInQueue(info->mHandler, info->mRunnable);

			auto& timerMap = mInternalData->mTimerMap;
			if (timerMap)
			{
				vector<long> timerIds;
				for (auto iter = timerMap->begin(); iter != timerMap->end(); ++iter)
				{
					auto item = iter->second->mExtraInfo;
					if (item && item->mRunnable == info->mRunnable)
					{
						timerIds.push_back(iter->first);
					}
				}

				for (auto iter = timerIds.begin(); iter != timerIds.end(); ++iter)
				{
					//DW("cancel runnable,id=%d",*iter);
					KillTimer(*iter);
				}
			}
		}

		return 0;
	}
	case BM_MESSAGE:
	{
		auto& obj = *(shared_ptr<Message>*)wp;
		HandleMessage(obj);
		return 0;
	}
	case BM_POST_MESSAGE:
	{
		auto p = (tagSelfRef*)wp;
		auto obj=p->mSelfRef;
		p->mSelfRef = nullptr;

		auto msg = dynamic_pointer_cast<Message>(obj->mObject);
		HandleMessage(msg);
		return 0;
	}
	default:
	{
		break;
	}
	}

	return 0;
}

long Handler::SetTimer(long& timerId,UINT interval)
{
	if (timerId)
	{
		KillTimer(timerId);
	}

	return timerId=mInternalData->SetTimer(interval);
}

void Handler::KillTimer(long& timerId)
{
	mInternalData->KillTimer(timerId);
}

//parent对child作弱引用
//child对parent作强引用
int Handler::AddChild(weak_ptr<Handler> child, string name)
{
	if (IsMyselfThread())
	{
		return mInternalData->AddChildHelper(child, name);
	}

	return (int)sendMessage(BM_ADD_CHILD, (WPARAM)&child, (LPARAM)name.c_str());
}

//避免在.h文件中引用Loop接口
int Handler::PostDisposeHelper(WPARAM wp)
{
	if (IsCreated())
	{
		mInternalData->mLooper->postMessage(BM_POST_DISPOSE, (WPARAM)wp);
		return 0;
	}
	else
	{
		DW("fail %s", __func__);
	}

	return -1;
}

void Handler::OnDestroy()
{
	ASSERT(IsMyselfThread());
	mInternalData->mTickDestroy = ShellTool::GetTickCount64();
	mInternalData->mOnDestroyCalled = true;

	MarkDestroyed();

	for (auto iter = mInternalData->mChildren.begin(); iter != mInternalData->mChildren.end();)
	{
		auto child = iter->second.lock();
		if (child)
		{
			if (child->IsLooper())
			{
				auto looper = dynamic_pointer_cast<Looper>(child);
				looper->PostQuitMessage();
			}
			else
			{
				child->sendMessage(BM_DESTROY);//这里不能使用postMessage,原因:不能保证在处理post message时的child有效性
			}

			++iter;
		}
		else
		{
			auto iterSave = iter;
			++iter;
			mInternalData->mChildren.erase(iterSave);
		}
	}

	if (IsLooper())
	{
		//looper mSelfRef是由looper thread entry特殊处理的
		//do nothing here
	}
	else
	{
		if (mInternalData->mPassive && mInternalData->mSelfRef)
		{
			PostDispose(mInternalData->mSelfRef);
		}

		mInternalData->mLooper->sendMessage(BM_HANDLER_DESTROY, (WPARAM)this);

	}
}

//说明:
//对Handler来说,主动调用Create()没太大意义,并且很容易忘记调用它
//所以约定BM_CREATE是可选的
//考虑到一般都会调用AddChild,所以在AddChild()中会适时触发BM_CREATE
//注意有可能跨looper来调用Create或AddChild,所以BM_CREATE都采用sendMessage来触发
void Handler::Create(shared_ptr<Handler> parent)
{
	auto ret = -1;
	if (parent)
	{
		ret = parent->AddChild(shared_from_this());
	}

	if (ret==0 && !mInternalData->mCreated)
	{
		sendMessage(BM_CREATE);
	}
}

//本接口用来初始化
//所有的Handler子类，都可在OnCreate()中初始化
void Handler::OnCreate()
{
	//LogV("%s::OnCreate",GetObjectName().c_str());
	mInternalData->mOnCreateCalled = true;
}

Loop *Handler::GetCurrentLooper()
{
	return Looper::CurrentLooper();
}

//根据name来返回mChildren中匹配的子对象,不递归
shared_ptr<Handler> Handler::GetChild(string name, Handler *afterHandler)
{
	if (!IsCreated())
	{
		return nullptr;
	}

	ASSERT(IsMyselfThread());

	bool needAfter = (afterHandler!=nullptr);
	for (auto iter = mInternalData->mChildren.begin(); iter != mInternalData->mChildren.end(); ++iter)
	{
		auto child = iter->second.lock();
		if (!child)
		{
			continue;
		}
		
		if (needAfter && child.get() != afterHandler)
		{
			continue;
		}

		if (child->GetObjectName() == name)
		{
			return child;
		}
	}

	return nullptr;
}

//返回指定id的child,不递归
shared_ptr<Handler> Handler::GetChild(LONG_PTR id)
{
	if (!IsCreated())
	{
		return nullptr;
	}

	ASSERT(id);
	if (IsMyselfThread())
	{
		return mInternalData->GetChild_Impl(id);
	}

	tagFindObjectInfo info;
	sendMessage(BM_GET_CHILD, (WPARAM)&info, (LPARAM)id);
	return info.mHandler;
}

int Handler::RegisterShortcut(const string& name, weak_ptr<Handler> obj)
{
	if (!IsCreated())
	{
		return -1;
	}

	return (int)sendMessage(BM_REGISTER_SHORTCUT, (WPARAM)&name,(LPARAM)&obj);
}

std::shared_ptr<Handler> Handler::Shortcut(const string& name)
{
	if (IsMyselfThread())
	{
		return mInternalData->Shortcut_Impl(name);
	}

	tagGetShortcut info;
	sendMessage(BM_GET_SHORTCUT, (WPARAM)&info, (LPARAM)&name);
	return info.mHandler;
}


//规定url中多层采用/分隔
shared_ptr<Handler> Handler::FindObject(const string& url)
{
	if (!IsCreated())
	{
		return nullptr;
	}

	if (IsMyselfThread())
	{
		return mInternalData->FindObject_Impl(url);
	}

	tagFindObjectInfo info;
	sendMessage(BM_FIND_OBJECT, (WPARAM)&info,(LPARAM)&url);
	return info.mHandler;
}

string Handler::GetUrl()const
{
	if (!IsCreated())
	{
		return "";
	}

	string url= GetObjectName();
	auto parent = mInternalData->mParent;
	while (parent)
	{
		url = parent->GetObjectName() + "/" + url;
		if (parent->IsLooper())
		{
			break;
		}

		parent = parent->mInternalData->mParent;
	}

	return url;
}

void Handler::Destroy()
{
	if (!IsCreated())
	{
		return;
	}

	if (!mInternalData->mDestroy)
	{
		mInternalData->mDestroy = true;
		sendMessage(BM_DESTROY);
	}
}

//原生looper中调用时,可成功取消用postRunnable和postDelayedRunnable投递并且还没执行的Runnable
//跨looper调用时，无法取消postRunnable投递的Runnable,可以取消用postDelayedRunnable投递并且还没执行的Runnable
//最高效的办法是在Runnable子类中用bool mCancel变量标记，在Run()中检查mCancel为true时直接返回，也能达到取消的效果
void Handler::cancelRunnable(shared_ptr<Runnable> obj)
{
	if (!IsCreated() || !obj)
	{
		return;
	}

	tagCancelRunnable info;
	info.mHandler = shared_from_this();
	info.mRunnable = obj;
	sendMessage(BM_CANCEL_RUNNABLE, (WPARAM)&info);
}

LRESULT Handler::postRunnable(shared_ptr<Runnable> obj)
{
	if (!IsCreated() || !obj)
	{
		DW("fail %s,IsCreated()=%d",__func__, IsCreated());
		return -1;
	}

	//发现直接投递的Runnable没法用cancelRunnable来取消
	//为了支持cancel,改用postDelayedRunnable延时来做

	auto info = make_shared<tagDelayedRunnable>();
	info->mHandler = shared_from_this();
	info->mRunnable = obj;
	info->mSelfRef = info;
	auto ret=postMessage(BM_POST_RUNNABLE, (WPARAM)info.get());
	if (ret)
	{
		info->mSelfRef = nullptr;
	}
	return ret;
}

//obj如果没有外部引用,在Runnable执行后或者RemoveAllTimer时会被销毁
LRESULT Handler::postDelayedRunnable(shared_ptr<Runnable> obj, UINT ms)
{
	if (!IsCreated() || !obj)
	{
		DW("%s fail",__func__);
		ASSERT(FALSE);
		return -1;
	}

	auto info = make_shared<tagDelayedRunnable>();
	info->mSelfRef = info;
	info->mHandler = shared_from_this();
	info->mRunnable = obj;
	info->mDelayedMS = ms;

	auto ret=postMessage(BM_POST_RUNNABLE,(WPARAM)info.get());
	if (ret)
	{
		info->mSelfRef = nullptr;

		DW("%s fail", __func__);
		ASSERT(FALSE);
	}

	return ret;
}

bool Handler::IsCreated()const
{
	return mInternalData->mCreated;
}

bool Handler::IsDestroyed()const
{
	return mInternalData->mDestroy;
}

bool Handler::MaybeLongBlock()const
{
	return mInternalData->mMaybeLongBlock;
}

shared_ptr<Handler> Handler::GetParent()const 
{
	return mInternalData->mParent;
}

LONG_PTR Handler::GetId()const
{
	return mInternalData->mId;
}

void Handler::SetId(LONG_PTR id)
{
	mInternalData->mId = id;
}

bool Handler::IsLooper()const
{
	return mInternalData->mIsLooper;
}

LPVOID Handler::GetLooperHandle()
{
	if (IsCreated() && mInternalData->mLooper)
	{
		return mInternalData->mLooper->GetLooperHandle();
	}

	return nullptr;
}

void Handler::MarkDestroyed()
{
	mInternalData->mDestroy = true;
}

void Handler::GetChildren(std::unordered_map<long*, weak_ptr<Handler>>& items)
{
	items=mInternalData->mChildren;
}

UINT Handler::BindMessage_(PFN_OnMessage entry)
{
	auto msg = AllocMessageId();
	BindMessageEx_(msg, entry);
	return msg;
}

int Handler::BindMessageEx_(UINT msg, PFN_OnMessage entry)
{
	if (msg < BM_NULL)
	{
		mInternalData->mMessageEntries[msg] = entry;
		return 0;
	}

	return -1;
};

//返回BM_NULL表示失败
UINT Handler::AllocMessageId()
{
	if (mInternalData->mNextAllocMessageId < BM_NULL)
	{
		return mInternalData->mNextAllocMessageId++;
	}

	return BM_NULL;
}

void Handler::HandleMessage(shared_ptr<Message> obj)
{
	UNUSED(obj);
}

string Handler::GetObjectName()const
{
	return mInternalData->GetName();
}

void Handler::SetObjectName(const string& name)
{
	mInternalData->SetName(name);
}

void Handler::OnPrepareDestructor()
{
	ASSERT(IsMyselfThread());

	mInternalData->OnPrepareDestructor();
}

#ifdef _DEBUG

void HandlerDebug::DumpAll()
{
	tagHandlerInternalData::DumpAll();
}
#endif

}
}

