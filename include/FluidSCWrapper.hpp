#pragma once

#include "SCBufferAdaptor.hpp"
#include <clients/common/FluidBaseClient.hpp>
#include <clients/common/Result.hpp>
#include <data/FluidTensor.hpp>
#include <data/TensorTypes.hpp>

#include <SC_PlugIn.hpp>

#include <algorithm>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>


namespace fluid {
namespace client {

template <typename Client>
class FluidSCWrapper;

namespace impl {

  template <size_t N, typename T>
  struct AssignBuffer
  {
    void operator()(const typename BufferT::type &p, World *w)
    {
      if (auto b = static_cast<SCBufferAdaptor *>(p.get()))
          b->assignToRT(w);
    }
  };

  template <size_t N, typename T>
  struct CleanUpBuffer
  {
    void operator()(const typename BufferT::type &p)
    {
      if (auto b = static_cast<SCBufferAdaptor *>(p.get())) b->cleanUp();
    }
  };


// Iterate over kr/ir inputs via callbacks from params object
struct FloatControlsIter
{
  FloatControlsIter(float **vals, size_t N)
  : mValues(vals)
  , mSize(N)
  {}
    
  float next() { return mCount >= mSize ? 0 : *mValues[mCount++]; }
    
  void reset(float **vals)
  {
    mValues = vals;
    mCount  = 0;
  }
    
  size_t size() const noexcept { return mSize; }
  size_t remain()
  {
    return mSize - mCount;
  }
private:
  float **mValues;
  size_t  mSize;
  size_t  mCount{0};
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    
// Real Time Processor

template <typename Client, class Wrapper>
class RealTime : public SCUnit
{
  using HostVector = FluidTensorView<float, 1>;
  using ParamSetType = typename Client::ParamSetType;
    
  //  using Client     = typename Wrapper::ClientType;

public:
  static void setup(InterfaceTable *ft, const char *name)
  {
    registerUnit<Wrapper>(ft, name);
    ft->fDefineUnitCmd(name, "latency", doLatency);
  }

  static void doLatency(Unit *unit, sc_msg_iter*)
  {
    float l[]{static_cast<float>(static_cast<Wrapper *>(unit)->mClient.latency())};
    auto  ft = Wrapper::getInterfaceTable();

    std::stringstream ss;
    ss << '/' << Wrapper::getName() << "_latency";
    std::cout << ss.str() << '\n';
    ft->fSendNodeReply(&unit->mParent->mNode, -1, ss.str().c_str(), 1, l);
  }
    
  RealTime()
    : mControlsIterator{mInBuf + mSpecialIndex + 1,static_cast<size_t>(static_cast<ptrdiff_t>(mNumInputs) - mSpecialIndex - 1)}
    , mParams{Wrapper::Client::getParameterDescriptors()}
    , mClient{Wrapper::setParams(mParams,mWorld->mVerbosity > 0, mWorld, mControlsIterator,true)}
  {}

  void init()
  {
    assert(!(mClient.audioChannelsOut() > 0 && mClient.controlChannelsOut() > 0) &&
           "Client can't have both audio and control outputs");

    mClient.sampleRate(fullSampleRate());
    mInputConnections.reserve(mClient.audioChannelsIn());
    mOutputConnections.reserve(mClient.audioChannelsOut());
    mAudioInputs.reserve(mClient.audioChannelsIn());
    mOutputs.reserve(std::max(mClient.audioChannelsOut(), mClient.controlChannelsOut()));

    for (int i = 0; i < static_cast<int>(mClient.audioChannelsIn()); ++i)
    {
      mInputConnections.emplace_back(isAudioRateIn(i));
      mAudioInputs.emplace_back(nullptr, 0, 0);
    }

    for (int i = 0; i < static_cast<int>(mClient.audioChannelsOut()); ++i)
    {
      mOutputConnections.emplace_back(true);
      mOutputs.emplace_back(nullptr, 0, 0);
    }
    
    for (int i = 0; i < static_cast<int>(mClient.controlChannelsOut()); ++i) { mOutputs.emplace_back(nullptr, 0, 0); }
    
    mCalcFunc = make_calc_function<RealTime, &RealTime::next>();
    Wrapper::getInterfaceTable()->fClearUnitOutputs(this, 1);
  }

  void next(int)
  {
    mControlsIterator.reset(mInBuf + mSpecialIndex + 1); //mClient.audioChannelsIn());
    Wrapper::setParams(mParams, mWorld->mVerbosity > 0, mWorld, mControlsIterator); // forward on inputs N + audio inputs as params
    mParams.constrainParameterValues(); 
    const Unit *unit = this;
    for (size_t i = 0; i < mClient.audioChannelsIn(); ++i)
    {
      if (mInputConnections[i]) mAudioInputs[i].reset(IN(i), 0, fullBufferSize());
    }
    for (size_t i = 0; i < mClient.audioChannelsOut(); ++i)
    {
      if (mOutputConnections[i]) mOutputs[i].reset(out(static_cast<int>(i)), 0, fullBufferSize());
    }
    for (size_t i = 0; i < mClient.controlChannelsOut(); ++i) { mOutputs[i].reset(out(static_cast<int>(i)), 0, 1); }
    mClient.process(mAudioInputs, mOutputs,mContext);
  }

private:
  std::vector<bool>       mInputConnections;
  std::vector<bool>       mOutputConnections;
  std::vector<HostVector> mAudioInputs;
  std::vector<HostVector> mOutputs;
  FloatControlsIter       mControlsIterator;
  FluidContext            mContext;

protected:
  ParamSetType  mParams;
  Client        mClient;
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/// Non Real Time Processor
/// This is also a UGen, but the main action is delegated off to a worker thread, via the NRT thread.
/// The RT bit is there to allow us (a) to poll our thread and (b) emit a kr progress update
template <typename Client, typename Wrapper>
class NonRealTime: public SCUnit
{
  using ParamSetType = typename Client::ParamSetType;
  
public:

  static void setup(InterfaceTable *ft, const char *name)
  {
    registerUnit<Wrapper>(ft, name);
    ft->fDefineUnitCmd(name, "cancel", doCancel);
    ft->fDefineUnitCmd(name, "queue_enabled", [](struct Unit* unit, struct sc_msg_iter* args)
    {
      auto w = static_cast<Wrapper *>(unit);
      w->mQueueEnabled = args->geti(0);
      w->mFifoMsg.Set(w->mWorld,[](FifoMsg* f)
      {
        auto w = static_cast<Wrapper*>(f->mData);
        w->mClient.setQueueEnabled(w->mQueueEnabled);
      },nullptr,w);
      Wrapper::getInterfaceTable()->fSendMsgFromRT(w->mWorld, w->mFifoMsg);
    });
    ft->fDefineUnitCmd(name, "synchronous", [](struct Unit* unit, struct sc_msg_iter* args)
    {
      auto w = static_cast<Wrapper *>(unit);
      w->mSynchronous = args->geti(0);
      w->mFifoMsg.Set(w->mWorld,[](FifoMsg* f)
      {
        auto w = static_cast<Wrapper*>(f->mData);
        w->mClient.setSynchronous(w->mSynchronous);
      },nullptr,w);
      Wrapper::getInterfaceTable()->fSendMsgFromRT(w->mWorld, w->mFifoMsg);
    });
  }
  
  /// Penultimate input is the doneAction, final is blocking mode. Neither are params, so we skip them in the controlsIterator
  NonRealTime() :
      mControlsIterator{mInBuf,static_cast<size_t>(static_cast<ptrdiff_t>(mNumInputs) - mSpecialIndex - 2)}
    , mParams{Wrapper::Client::getParameterDescriptors()}
    , mClient{Wrapper::setParams(mParams,mWorld->mVerbosity > 0, mWorld, mControlsIterator,true)}
    , mSynchronous{mNumInputs > 2 ? (in0(static_cast<int>(mNumInputs - 1)) > 0) : false}
  {}
  
  ~NonRealTime()
  {
    if(mClient.state() == ProcessState::kProcessing)
    {
      std::cout << Wrapper::getName() << ": Processing cancelled \n";
      Wrapper::getInterfaceTable()->fSendNodeReply(&mParent->mNode,1,"/done",0,nullptr);
    }
    //processing will be cancelled in ~NRTThreadAdaptor()
  }
  

  /// No option of not using a worker thread for now
  /// init() sets up the NRT process via the SC NRT thread, and then sets our UGen calc function going
  void init()
  {
    mFifoMsg.Set(mWorld, initNRTJob, nullptr, this);
    mWorld->ft->fSendMsgFromRT(mWorld,mFifoMsg);    
    //we want to poll thread roughly every 20ms
    checkThreadInterval = static_cast<size_t>(0.02 / controlDur());
    set_calc_function<NonRealTime, &NonRealTime::poll>();
  };
  
  /// The calc function. Checks to see if we've cancelled, spits out progress, launches tidy up when complete
  void poll(int)
  {
    out0(0) = mDone ? 1.0 : static_cast<float>(mClient.progress());

    if(0 == pollCounter++ && !mCheckingForDone)
    {
      mCheckingForDone = true;
      mWorld->ft->fDoAsynchronousCommand(mWorld, nullptr, Wrapper::getName(), this,
                              postProcess, exchangeBuffers, tidyUp, destroy,
                              0, nullptr);
    }
    pollCounter %= checkThreadInterval;
  }
  

  /// To be called on NRT thread. Validate parameters and commence processing in new thread
  static void initNRTJob(FifoMsg* f)
  {
    auto w = static_cast<Wrapper*>(f->mData);
    w->mDone = false;
    w->mCancelled = false;

    Result result = validateParameters(w);
    
    if (!result.ok())
    {
        std::cout << "ERROR: " << Wrapper::getName() << ": " << result.message().c_str() << std::endl;
        return;
    }
    w->mClient.setSynchronous(w->mSynchronous); 
    w->mClient.enqueue(w->mParams);
    w->mClient.process();
  }

  /// Check result and report if bad
  static bool postProcess(World*, void *data)
  {
    auto w = static_cast<Wrapper*>(data);
    Result r;
    ProcessState s = w->mClient.checkProgress(r);
    
    if((s==ProcessState::kDone || s==ProcessState::kDoneStillProcessing)
      || (w->mSynchronous && s==ProcessState::kNoProcess) ) //I think this hinges on the fact that when mSynchrous = true, this call will always be behind process() on the command FIFO, so we can assume that if the state is kNoProcess, it has run (vs never having run) 
    {
      //Given that cancellation from the language now always happens by freeing the
      //synth, this block isn't reached normally. HOwever, if someone cancels using u_cmd, this is what will fire
      if(r.status() == Result::Status::kCancelled)
      {
        std::cout <<  Wrapper::getName() << ": Processing cancelled \n";
        w->mCancelled = true;
        return false;
      }
      
      if(!r.ok())
      {
        std::cout << "ERROR: " << Wrapper::getName() << ": " << r.message().c_str() << '\n';
        return false;
      }
      
      w->mDone = true;
      return true;
    }
    return false;
  }

  /// swap NRT buffers back to RT-land
  static bool exchangeBuffers(World *world, void *data) { return static_cast<Wrapper *>(data)->exchangeBuffers(world); }
  /// Tidy up any temporary buffers
  static bool tidyUp(World *world, void *data) { return static_cast<Wrapper *>(data)->tidyUp(world); }
  
  /// Now we're actually properly done, call the UGen's done action (possibly destroying this instance)
  static void destroy(World* world, void* data)
  {
    auto w = static_cast<Wrapper*>(data);
    if(w->mDone && w->mNumInputs > 2) //don't check for doneAction if UGen has no ins (there should be 3 minimum -> sig, doneAction, blocking mode)
    {
      int doneAction = static_cast<int>(w->in0(static_cast<int>(w->mNumInputs - 2))); //doneAction is penultimate input; THIS IS THE LAW
      world->ft->fDoneAction(doneAction,w);
      return;
    }
    w->mCheckingForDone = false;
  }
  
  static void doCancel(Unit *unit, sc_msg_iter*)
  {
    static_cast<Wrapper *>(unit)->mClient.cancel();
  }
private:
    
  static Result validateParameters(NonRealTime *w)
  {
    auto results = w->mParams.constrainParameterValues();
    for (auto &r : results)
    {
      if (!r.ok()) return r;
    }
    return {};
  }

  bool exchangeBuffers(World *world) //RT thread
  {
    mParams.template forEachParamType<BufferT, impl::AssignBuffer>(world);
    //At this point, we can see if we're finished and let the language know (or it can wait for the doneAction, but that takes extra time)
    //use replyID to convey status (0 = normal completion, 1 = cancelled)
    if(mDone)      world->ft->fSendNodeReply(&mParent->mNode,0,"/done",0,nullptr);
    if(mCancelled) world->ft->fSendNodeReply(&mParent->mNode,1,"/done",0,nullptr);
    return true;
  }

  bool tidyUp(World *) //NRT thread
  {
    mParams.template forEachParamType<BufferT, impl::CleanUpBuffer>();
    return true;
  }


  
  FloatControlsIter       mControlsIterator;
  FifoMsg     mFifoMsg;
  char*       mCompletionMessage = nullptr;
  void*       mReplyAddr         = nullptr;
  const char *mName              = nullptr;
  size_t      checkThreadInterval;
  size_t      pollCounter{0};
protected:
  ParamSetType  mParams;
  Client        mClient;
  bool          mSynchronous{true};
  bool          mQueueEnabled{false};
  bool          mCheckingForDone{false}; //only write to this from RT thread kthx
  bool          mCancelled{false};
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    
/// An impossible monstrosty
template <typename Client, typename Wrapper>
class NonRealTimeAndRealTime : public RealTime<Client, Wrapper>, public NonRealTime<Client, Wrapper>
{
  static void setup(InterfaceTable *ft, const char *name)
  {
    RealTime<Client,Wrapper>::setup(ft, name);
    NonRealTime<Client,Wrapper>::setup(ft, name);
  }
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    
// Template Specialisations for NRT/RT

template <typename Client, typename Wrapper, typename NRT, typename RT>
class FluidSCWrapperImpl;

template <typename Client, typename Wrapper>
class FluidSCWrapperImpl<Client, Wrapper, std::true_type, std::false_type>
    : public NonRealTime<Client, Wrapper>
{
//public:
//  FluidSCWrapperImpl(World* w, sc_msg_iter *args): NonRealTime<Client, Wrapper>(w,args){};
};

template <typename Client, typename Wrapper>
class FluidSCWrapperImpl<Client, Wrapper, std::false_type, std::true_type> : public RealTime<Client, Wrapper>
{};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
      
// Make base class(es), full of CRTP mixin goodness
template <typename Client>
using FluidSCWrapperBase = FluidSCWrapperImpl<Client, FluidSCWrapper<Client>, typename Client::isNonRealTime, typename Client::isRealTime>;

} // namespace impl

////////////////////////////////////////////////////////////////////////////////////////////////////////////////

///The main wrapper
template <typename C>
class FluidSCWrapper : public impl::FluidSCWrapperBase<C>
{

  using FloatControlsIter = impl::FloatControlsIter;
  
  template <typename ArgType>
  struct ParamReader
  {

    static auto fromArgs(World *, sc_msg_iter* args, std::string, int)
    {      
      const char* recv = args->gets("");
      
      return std::string(recv?recv:"");
    }

    static auto fromArgs(World *w, FloatControlsIter& args, std::string, int)
    {
        //first is string size, then chars
        int size = static_cast<int>(args.next());
        char* chunk =  static_cast<char*>(FluidSCWrapper::getInterfaceTable()->fRTAlloc(w,size + 1));
      
        if (!chunk) {
          std::cout << "ERROR: " << FluidSCWrapper::getName() << ": RT memory allocation failed\n";
          return std::string{""};
        }
      
        for(int i = 0; i < size; ++i)
          chunk[i] = static_cast<char>(args.next());
        
        chunk[size] = 0; //terminate string
        
        return std::string{chunk};
    }
    
    
    template<typename T>
    static std::enable_if_t<std::is_integral<T>::value,T>
    fromArgs(World *, FloatControlsIter& args, T, int) { return args.next(); }
    
    template<typename T>
    static std::enable_if_t<std::is_floating_point<T>::value,T>
    fromArgs(World *, FloatControlsIter& args, T, int) { return args.next(); }
    
    template<typename T>
    static std::enable_if_t<std::is_integral<T>::value,T>
    fromArgs(World *, sc_msg_iter* args, T, int defVal) { return args->geti(defVal); }

    template<typename T>
    static std::enable_if_t<std::is_floating_point<T>::value,T>
    fromArgs(World *, sc_msg_iter* args, T, int) { return args->getf(); }

    static auto fromArgs(World *w, ArgType args, BufferT::type&, int)
    {
      typename LongT::type bufnum = static_cast<typename LongT::type>(ParamReader::fromArgs(w, args, typename LongT::type(), -1));
      return BufferT::type(bufnum >= 0 ? new SCBufferAdaptor(bufnum, w) : nullptr);
    }
    
    static auto fromArgs(World *w, ArgType args, InputBufferT::type&, int)
    {
      typename LongT::type bufnum = static_cast<LongT::type>(fromArgs(w, args, LongT::type(), -1));
      return InputBufferT::type(bufnum >= 0 ? new SCBufferAdaptor(bufnum, w) : nullptr);
    }
    
    template<typename P>
    static std::enable_if_t<IsSharedClient<P>::value,P>
    fromArgs(World *w, ArgType args, P&, int)
    {
        return {fromArgs(w, args, std::string{}, 0).c_str()};
    }
    
  };
  
  
  
  // Iterate over arguments via callbacks from params object
  template <typename ArgType, size_t N, typename T>
  struct Setter
  {
    static constexpr size_t argSize = C::getParameterDescriptors().template get<N>().fixedSize;
    
    typename T::type operator()(World *w, ArgType args)
    {
      //Just return default if there's nothing left to grab
      if(args.remain() ==  0)
      {
        std::cout << "WARNING: " << getName() << " received fewer parameters than expected\n";
        return C::getParameterDescriptors().template makeValue<N>();
      }
      
      ParamLiteralConvertor<T, argSize> a;
      using LiteralType = typename ParamLiteralConvertor<T, argSize>::LiteralType;

      for (size_t i = 0; i < argSize; i++)
        a[i] = static_cast<LiteralType>(ParamReader<ArgType>::fromArgs(w, args, a[0], 0));

      return a.value();
    }
  };
  
  template <size_t N, typename T>
  using ArgumentSetter = Setter<sc_msg_iter*, N, T>;

  template <size_t N, typename T>
  using ControlSetter = Setter<FloatControlsIter&, N, T>;
  
  //CryingEmoji.png: SC API hides all the useful functions for sending
  //replies back to the language with things like, uh, strings and stuff.
  //We have Node_SendReply, which assumes you are sending an array of float,
  //and must be called only from the RT thread. Thanks.
  //So, we do in reverse what the SendReply Ugen does, and parse
  //an array of floats as characters in the language. VomitEmoji.png
  
  struct ToFloatArray
  {
    static size_t allocSize(typename BufferT::type){ return 1; }
    
    template<typename T>
    static std::enable_if_t<std::is_integral<T>::value||std::is_floating_point<T>::value,size_t>
    allocSize(T){ return 1; }
    
    static size_t allocSize(std::string s){ return s.size() + 1; } //put null char at end when we send
    
    static size_t allocSize(FluidTensor<std::string,1> s)
    {
      size_t count = 0;
      for(auto& str: s) count += (str.size() + 1);
      return count;
    }
    template<typename T>
    static size_t allocSize(FluidTensor<T,1> s) { return s.size() ; }
    
    template<typename...Ts>
    static std::tuple<std::array<size_t,sizeof...(Ts)>,size_t> allocSize(std::tuple<Ts...>&& t)
    {
      return allocSizeImpl(std::forward<decltype(t)>(t), std::index_sequence_for<Ts...>());
    };
    
    template<typename...Ts, size_t...Is>
    static std::tuple<std::array<size_t,sizeof...(Ts)>,size_t> allocSizeImpl(std::tuple<Ts...>&& t,std::index_sequence<Is...>)
    {
      size_t size{0};
      std::array<size_t,sizeof...(Ts)> res;
      (void)std::initializer_list<int>{(res[Is] = size,size += ToFloatArray::allocSize(std::get<Is>(t)),0)...};
      return std::make_tuple(res,size); //array of offsets into allocated buffer & total number of floats to alloc
    };
    
    static void convert(float* f, typename BufferT::type buf) { f[0] = static_cast<SCBufferAdaptor*>(buf.get())->bufnum(); }
   
    template<typename T>
    static std::enable_if_t<std::is_integral<T>::value||std::is_floating_point<T>::value>
    convert(float* f, T x) { f[0] =  static_cast<float>(x); }
   
    static void convert(float* f, std::string s)
    {
      std::copy(s.begin(), s.end(), f);
      f[s.size()] = 0; //terminate
    }
    static void convert(float* f, FluidTensor<std::string,1> s)
    {
      for(auto& str: s)
      {
        std::copy(str.begin(), str.end(), f);
        f += str.size();
        *f++ = 0;
      }
    }
    template<typename T>
    static void convert(float* f, FluidTensor<T,1> s)
    {
        static_assert(std::is_convertible<T,float>::value,"Can't convert this to float output");
        std::copy(s.begin(), s.end(), f);
    }
    
    template<typename...Ts, size_t...Is>
    static void convert(float* f,std::tuple<Ts...>&& t, std::array<size_t,sizeof...(Ts)> offsets, std::index_sequence<Is...>)
    {
        (void)std::initializer_list<int>{(convert(f + offsets[Is],std::get<Is>(t)),0)...};
    }
  };
  
  
  //So, to handle a message to a plugin we will need to
  // (1) Launch the invovation of the message on the SC NRT Queue using FIFO Message
  // (2) Run the actual function (maybe asynchronously, in our own thread)
  // (3) Launch an asynchronous command to send the reply back (in Stage 3)
  
  template<size_t N, typename Ret, typename ArgTuple>
  struct MessageDispatch
  {
    static constexpr size_t Message = N;
    FluidSCWrapper* wrapper;
    ArgTuple args;
    Ret result;
    std::string name;
  };
  
  //Sets up a single /u_cmd
  template<size_t N, typename T>
  struct SetupMessage
  {
    void operator()(const T& message)
    {
        auto ft = getInterfaceTable();
        ft->fDefineUnitCmd(getName(), message.name, launchMessage<N>);
    }
  };

  template<size_t N>
  static void launchMessage(Unit* u,sc_msg_iter* args)
  {
    FluidSCWrapper* x = static_cast<FluidSCWrapper*>(u);
    using IndexList = typename Client::MessageSetType::template MessageDescriptorAt<N>::IndexList;
    launchMessageImpl<N>(x,args,IndexList());
  }

  template<size_t N, size_t...Is>
  static void launchMessageImpl(FluidSCWrapper* x,sc_msg_iter* inArgs,std::index_sequence<Is...>)
  {
    using MessageDescriptor = typename Client::MessageSetType::template MessageDescriptorAt<N>;
    using ArgTuple = typename MessageDescriptor::ArgumentTypes;
    using ReturnType = typename MessageDescriptor::ReturnType;
    using IndexList = typename MessageDescriptor::IndexList;
    using MessageData =  MessageDispatch<N,ReturnType,ArgTuple>;
    
    auto ft = getInterfaceTable();
    void* msgptr = ft->fRTAlloc(x->mWorld,sizeof(MessageData));
    MessageData* msg = new(msgptr) MessageData;
    ArgTuple& args = msg->args;
    (void)std::initializer_list<int>{(std::get<Is>(args) = ParamReader<sc_msg_iter*>::fromArgs(x->mWorld,inArgs,std::get<Is>(args),0),0)...};
    
    msg->name = '/' + Client::getMessageDescriptors().template name<N>();
    msg->wrapper = x;
    x->mDone = false;
    ft->fDoAsynchronousCommand(x->mWorld, nullptr, getName(), msg,
                              [](World*, void* data) //NRT thread: invocation
                              {
                                MessageData* m = static_cast<MessageData*>(data);
                                m->result = ReturnType{invokeImpl<N>(m->wrapper, m->args, IndexList{})};
                                
                                if(!m->result.ok())
                                {
                                  printResult(m->wrapper, m->result);
                                  return false;
                                }
                                return true;
                              },
                              [](World* world, void* data) //RT thread:  response
                              {
                                MessageData* m = static_cast<MessageData*>(data);
                                MessageDescriptor::template forEachArg<typename BufferT::type,impl::AssignBuffer>(m->args, world);
                                messageOutput(m->wrapper,m->name,m->result);
                                return true;
                              }
                              , nullptr, //NRT Thread: No-op
                              [](World* w, void* data) //RT thread: clean up
                              {
                                 getInterfaceTable()->fRTFree(w,data);
                              },
                              0, nullptr);
  }
  
  template <size_t N, typename ArgsTuple,size_t...Is> //Call from NRT
  static decltype(auto) invokeImpl(FluidSCWrapper* x, ArgsTuple& args, std::index_sequence<Is...>)
  {
    return x->mClient.template invoke<N>(x->mClient,std::get<Is>(args)...);
  }
  
  template<typename T> //call from RT
  static void messageOutput(FluidSCWrapper* x, const std::string& s,  MessageResult<T>& result)
  {
        auto ft = getInterfaceTable();
        //allocate return values
        size_t numArgs = ToFloatArray::allocSize(static_cast<T>(result));
        float* values = static_cast<float*>(ft->fRTAlloc(x->mWorld,numArgs * sizeof(float)));
        //copy return data
        ToFloatArray::convert(values,static_cast<T>(result));
        ft->fSendNodeReply(&x->mParent->mNode, -1, s.c_str(), static_cast<int>(numArgs), values);
  }
  
  static void messageOutput(FluidSCWrapper* x, const std::string& s,  MessageResult<void>&)
  {
      auto ft = getInterfaceTable();
      ft->fSendNodeReply(&x->mParent->mNode, -1, s.c_str(), 0, nullptr);
  }
  
  template<typename...Ts>
  static void messageOutput(FluidSCWrapper* x, const std::string& s,  MessageResult<std::tuple<Ts...>>& result)
  {
    auto ft = getInterfaceTable();
    std::array<size_t,sizeof...(Ts)> offsets;
    size_t numArgs;
    std::tie(offsets,numArgs) = ToFloatArray::allocSize(static_cast<std::tuple<Ts...>>(result));
    float* values = static_cast<float*>(ft->fRTAlloc(x->mWorld,numArgs * sizeof(float)));
    ToFloatArray::convert(values,std::tuple<Ts...>(result),offsets,std::index_sequence_for<Ts...>());
    ft->fSendNodeReply(&x->mParent->mNode, -1, s.c_str(), static_cast<int>(numArgs), values);
  }
  
  
  
public:
  using Client = C;
  using ParameterSetType = typename C::ParamSetType;

  FluidSCWrapper()
  {
    impl::FluidSCWrapperBase<Client>::init();
  }

  static const char *getName(const char *setName = nullptr)
  {
    static const char *name = nullptr;
    return (name = setName ? setName : name);
  }

  static InterfaceTable *getInterfaceTable(InterfaceTable *setTable = nullptr)
  {
    static InterfaceTable *ft = nullptr;
    return (ft = setTable ? setTable : ft);
  }

  static void setup(InterfaceTable *ft, const char *name)
  {
    getName(name);
    getInterfaceTable(ft);
    impl::FluidSCWrapperBase<Client>::setup(ft, name);
    Client::getMessageDescriptors().template iterate<SetupMessage>(); 
  }

  static auto& setParams(ParameterSetType& p, bool verbose, World* world, FloatControlsIter& inputs, bool constrain = false)
  {
    p.template setParameterValues<ControlSetter>(verbose, world, inputs);
    if(inputs.remain() > 0) std::cout << "WARNING: "<< getName() << " received " << inputs.remain() << " more parameters than expected. Perhaps your binary plugins and SC sources are different versions\n";
    if (constrain) p.constrainParameterValues();
    return p;
  }
  
  static void printResult(FluidSCWrapper* x, Result& r)
  {
    if (!x) return;

    switch (r.status())
    {
      case Result::Status::kWarning:
      {
        if(x->mWorld->mVerbosity > 0)
          std::cout << "WARNING: "  << r.message().c_str() << '\n';
        break;
      }
      case Result::Status::kError:
      {
        std::cout << "ERROR: " << r.message().c_str() << '\n';
        break;
      }
      case Result::Status::kCancelled:
      {
        std::cout << "Task cancelled\n" << '\n';
        break;
      }
      default: {
      }
    }
  }
};

template <typename  Client>
void makeSCWrapper(const char *name, InterfaceTable *ft)
{
  FluidSCWrapper<Client>::setup(ft, name);
}

} // namespace client
} // namespace fluid
