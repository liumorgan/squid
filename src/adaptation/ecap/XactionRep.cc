/*
 * DEBUG: section 93    eCAP Interface
 */
#include "squid.h"
#include <libecap/common/area.h>
#include <libecap/common/delay.h>
#include <libecap/adapter/xaction.h>
#include "HttpRequest.h"
#include "HttpReply.h"
#include "SquidTime.h"
#include "adaptation/ecap/XactionRep.h"
#include "adaptation/Initiator.h"
#include "base/TextException.h"

CBDATA_NAMESPACED_CLASS_INIT(Adaptation::Ecap::XactionRep, XactionRep);


Adaptation::Ecap::XactionRep::XactionRep(
    HttpMsg *virginHeader, HttpRequest *virginCause,
    const Adaptation::ServicePointer &aService):
        AsyncJob("Adaptation::Ecap::XactionRep"),
        Adaptation::Initiate("Adaptation::Ecap::XactionRep"),
        theService(aService),
        theVirginRep(virginHeader), theCauseRep(NULL),
        makingVb(opUndecided), proxyingAb(opUndecided),
        adaptHistoryId(-1),
        vbProductionFinished(false),
        abProductionFinished(false), abProductionAtEnd(false)
{
    if (virginCause)
        theCauseRep = new MessageRep(virginCause);
}

Adaptation::Ecap::XactionRep::~XactionRep()
{
    assert(!theMaster);
    delete theCauseRep;
    theAnswerRep.reset();
}

void
Adaptation::Ecap::XactionRep::master(const AdapterXaction &x)
{
    Must(!theMaster);
    Must(x != NULL);
    theMaster = x;
}

Adaptation::Service &
Adaptation::Ecap::XactionRep::service()
{
    Must(theService != NULL);
    return *theService;
}

void
Adaptation::Ecap::XactionRep::start()
{
    Must(theMaster);

    if (!theVirginRep.raw().body_pipe)
        makingVb = opNever; // there is nothing to deliver

    const HttpRequest *request = dynamic_cast<const HttpRequest*> (theCauseRep ?
                                 theCauseRep->raw().header : theVirginRep.raw().header);
    Must(request);
    Adaptation::History::Pointer ah = request->adaptLogHistory();
    if (ah != NULL) {
        // retrying=false because ecap never retries transactions
        adaptHistoryId = ah->recordXactStart(service().cfg().key, current_time, false);
    }

    theMaster->start();
}

void
Adaptation::Ecap::XactionRep::swanSong()
{
    // clear body_pipes, if any
    // this code does not maintain proxying* and canAccessVb states; should it?

    if (theAnswerRep != NULL) {
        BodyPipe::Pointer body_pipe = answer().body_pipe;
        if (body_pipe != NULL) {
            Must(body_pipe->stillProducing(this));
            stopProducingFor(body_pipe, false);
        }
    }

    BodyPipe::Pointer &body_pipe = theVirginRep.raw().body_pipe;
    if (body_pipe != NULL && body_pipe->stillConsuming(this))
        stopConsumingFrom(body_pipe);

    terminateMaster();

    const HttpRequest *request = dynamic_cast<const HttpRequest*>(theCauseRep ?
                                 theCauseRep->raw().header : theVirginRep.raw().header);
    Must(request);
    Adaptation::History::Pointer ah = request->adaptLogHistory();
    if (ah != NULL && adaptHistoryId >= 0)
        ah->recordXactFinish(adaptHistoryId);

    Adaptation::Initiate::swanSong();
}

libecap::Message &
Adaptation::Ecap::XactionRep::virgin()
{
    return theVirginRep;
}

const libecap::Message &
Adaptation::Ecap::XactionRep::cause()
{
    Must(theCauseRep != NULL);
    return *theCauseRep;
}

libecap::Message &
Adaptation::Ecap::XactionRep::adapted()
{
    Must(theAnswerRep != NULL);
    return *theAnswerRep;
}

Adaptation::Message &
Adaptation::Ecap::XactionRep::answer()
{
    MessageRep *rep = dynamic_cast<MessageRep*>(theAnswerRep.get());
    Must(rep);
    return rep->raw();
}

void
Adaptation::Ecap::XactionRep::terminateMaster()
{
    if (theMaster) {
        AdapterXaction x = theMaster;
        theMaster.reset();
        x->stop();
    }
}

bool
Adaptation::Ecap::XactionRep::doneAll() const
{
    return makingVb >= opComplete && proxyingAb >= opComplete &&
           Adaptation::Initiate::doneAll();
}

// stops receiving virgin and enables auto-consumption, dropping any vb bytes
void
Adaptation::Ecap::XactionRep::sinkVb(const char *reason)
{
    debugs(93,4, HERE << "sink for " << reason << "; status:" << status());

    // we reset raw().body_pipe when we are done, so use this one for checking
    const BodyPipePointer &permPipe = theVirginRep.raw().header->body_pipe;
    if (permPipe != NULL)
        permPipe->enableAutoConsumption();

    forgetVb(reason);
}

// stops receiving virgin but preserves it for others to use
void
Adaptation::Ecap::XactionRep::preserveVb(const char *reason)
{
    debugs(93,4, HERE << "preserve for " << reason << "; status:" << status());

    // we reset raw().body_pipe when we are done, so use this one for checking
    const BodyPipePointer &permPipe = theVirginRep.raw().header->body_pipe;
    if (permPipe != NULL) {
        // if libecap consumed, we cannot preserve
        Must(!permPipe->consumedSize());
    }

    forgetVb(reason);
}

// disassociates us from vb; the last step of sinking or preserving vb
void
Adaptation::Ecap::XactionRep::forgetVb(const char *reason)
{
    debugs(93,9, HERE << "forget vb " << reason << "; status:" << status());

    BodyPipePointer &p = theVirginRep.raw().body_pipe;
    if (p != NULL && p->stillConsuming(this))
        stopConsumingFrom(p);

    if (makingVb == opUndecided)
        makingVb = opNever;
    else if (makingVb == opOn)
        makingVb = opComplete;
}

void
Adaptation::Ecap::XactionRep::useVirgin()
{
    debugs(93,3, HERE << status());
    Must(proxyingAb == opUndecided);
    proxyingAb = opNever;

    preserveVb("useVirgin");

    HttpMsg *clone = theVirginRep.raw().header->clone();
    // check that clone() copies the pipe so that we do not have to
    Must(!theVirginRep.raw().header->body_pipe == !clone->body_pipe);

    sendAnswer(Answer::Forward(clone));
    Must(done());
}

void
Adaptation::Ecap::XactionRep::useAdapted(const libecap::shared_ptr<libecap::Message> &m)
{
    debugs(93,3, HERE << status());
    Must(m);
    theAnswerRep = m;
    Must(proxyingAb == opUndecided);

    HttpMsg *msg = answer().header;
    if (!theAnswerRep->body()) { // final, bodyless answer
        proxyingAb = opNever;
        sendAnswer(Answer::Forward(msg));
    } else { // got answer headers but need to handle body
        proxyingAb = opOn;
        Must(!msg->body_pipe); // only host can set body pipes
        MessageRep *rep = dynamic_cast<MessageRep*>(theAnswerRep.get());
        Must(rep);
        rep->tieBody(this); // sets us as a producer
        Must(msg->body_pipe != NULL); // check tieBody

        sendAnswer(Answer::Forward(msg));

        debugs(93,4, HERE << "adapter will produce body" << status());
        theMaster->abMake(); // libecap will produce
    }
}

void
Adaptation::Ecap::XactionRep::blockVirgin()
{
    debugs(93,3, HERE << status());
    Must(proxyingAb == opUndecided);
    proxyingAb = opNever;

    sinkVb("blockVirgin");

    sendAnswer(Answer::Block(service().cfg().key));
    Must(done());
}

void
Adaptation::Ecap::XactionRep::vbDiscard()
{
    Must(makingVb == opUndecided);
    // if adapter does not need vb, we do not need to send it
    sinkVb("vbDiscard");
    Must(makingVb == opNever);
}

void
Adaptation::Ecap::XactionRep::vbMake()
{
    Must(makingVb == opUndecided);
    BodyPipePointer &p = theVirginRep.raw().body_pipe;
    Must(p != NULL);
    Must(p->setConsumerIfNotLate(this)); // to deliver vb, we must receive vb
    makingVb = opOn;
}

void
Adaptation::Ecap::XactionRep::vbStopMaking()
{
    Must(makingVb == opOn);
    // if adapter does not need vb, we do not need to receive it
    sinkVb("vbStopMaking");
    Must(makingVb == opComplete);
}

void
Adaptation::Ecap::XactionRep::vbMakeMore()
{
    Must(makingVb == opOn); // cannot make more if done proxying
    // we cannot guarantee more vb, but we can check that there is a chance
    const BodyPipePointer &p = theVirginRep.raw().body_pipe;
    Must(p != NULL && p->stillConsuming(this)); // we are plugged in
    Must(!p->productionEnded() && p->mayNeedMoreData()); // and may get more
}

libecap::Area
Adaptation::Ecap::XactionRep::vbContent(libecap::size_type o, libecap::size_type s)
{
    // We may not be makingVb yet. It should be OK, but see vbContentShift().

    const BodyPipePointer &p = theVirginRep.raw().body_pipe;
    Must(p != NULL);

    // TODO: make MemBuf use size_t?
    const size_t haveSize = static_cast<size_t>(p->buf().contentSize());

    // convert to Squid types; XXX: check for overflow
    const uint64_t offset = static_cast<uint64_t>(o);
    Must(offset <= haveSize); // equal iff at the end of content

    // nsize means no size limit: all content starting from offset
    const size_t size = s == libecap::nsize ?
                        haveSize - offset : static_cast<size_t>(s);

    // XXX: optimize by making theBody a shared_ptr (see Area::FromTemp*() src)
    return libecap::Area::FromTempBuffer(p->buf().content() + offset,
                                         min(static_cast<size_t>(haveSize - offset), size));
}

void
Adaptation::Ecap::XactionRep::vbContentShift(libecap::size_type n)
{
    // We may not be makingVb yet. It should be OK now, but if BodyPipe
    // consume() requirements change, we would have to return empty vbContent
    // until the adapter registers as a consumer

    BodyPipePointer &p = theVirginRep.raw().body_pipe;
    Must(p != NULL);
    const size_t size = static_cast<size_t>(n); // XXX: check for overflow
    const size_t haveSize = static_cast<size_t>(p->buf().contentSize()); // TODO: make MemBuf use size_t?
    p->consume(min(size, haveSize));
}

void
Adaptation::Ecap::XactionRep::noteAbContentDone(bool atEnd)
{
    Must(proxyingAb == opOn && !abProductionFinished);
    abProductionFinished = true;
    abProductionAtEnd = atEnd; // store until ready to stop producing ourselves
    debugs(93,5, HERE << "adapted body production ended");
    moveAbContent();
}

void
Adaptation::Ecap::XactionRep::noteAbContentAvailable()
{
    Must(proxyingAb == opOn && !abProductionFinished);
    moveAbContent();
}

#if 0 /* XXX: implement */
void
Adaptation::Ecap::XactionRep::setAdaptedBodySize(const libecap::BodySize &size)
{
    Must(answer().body_pipe != NULL);
    if (size.known())
        answer().body_pipe->setBodySize(size.value());
    // else the piped body size is unknown by default
}
#endif

void
Adaptation::Ecap::XactionRep::adaptationDelayed(const libecap::Delay &d)
{
    debugs(93,3, HERE << "adapter needs time: " <<
           d.state << '/' << d.progress);
    // XXX: set timeout?
}

void
Adaptation::Ecap::XactionRep::adaptationAborted()
{
    tellQueryAborted(true); // should eCAP support retries?
    mustStop("adaptationAborted");
}

bool
Adaptation::Ecap::XactionRep::callable() const
{
    return !done();
}

void
Adaptation::Ecap::XactionRep::noteMoreBodySpaceAvailable(RefCount<BodyPipe> bp)
{
    Must(proxyingAb == opOn);
    moveAbContent();
}

void
Adaptation::Ecap::XactionRep::noteBodyConsumerAborted(RefCount<BodyPipe> bp)
{
    Must(proxyingAb == opOn);
    stopProducingFor(answer().body_pipe, false);
    Must(theMaster);
    theMaster->abStopMaking();
    proxyingAb = opComplete;
}

void
Adaptation::Ecap::XactionRep::noteMoreBodyDataAvailable(RefCount<BodyPipe> bp)
{
    Must(makingVb == opOn); // or we would not be registered as a consumer
    Must(theMaster);
    theMaster->noteVbContentAvailable();
}

void
Adaptation::Ecap::XactionRep::noteBodyProductionEnded(RefCount<BodyPipe> bp)
{
    Must(makingVb == opOn); // or we would not be registered as a consumer
    Must(theMaster);
    theMaster->noteVbContentDone(true);
    vbProductionFinished = true;
}

void
Adaptation::Ecap::XactionRep::noteBodyProducerAborted(RefCount<BodyPipe> bp)
{
    Must(makingVb == opOn); // or we would not be registered as a consumer
    Must(theMaster);
    theMaster->noteVbContentDone(false);
    vbProductionFinished = true;
}

void
Adaptation::Ecap::XactionRep::noteInitiatorAborted()
{
    mustStop("initiator aborted");
}

// get content from the adapter and put it into the adapted pipe
void
Adaptation::Ecap::XactionRep::moveAbContent()
{
    Must(proxyingAb == opOn);
    const libecap::Area c = theMaster->abContent(0, libecap::nsize);
    debugs(93,5, HERE << "up to " << c.size << " bytes");
    if (c.size == 0 && abProductionFinished) { // no ab now and in the future
        stopProducingFor(answer().body_pipe, abProductionAtEnd);
        proxyingAb = opComplete;
        debugs(93,5, HERE << "last adapted body data retrieved");
    } else if (c.size > 0) {
        if (const size_t used = answer().body_pipe->putMoreData(c.start, c.size))
            theMaster->abContentShift(used);
    }
}

const char *
Adaptation::Ecap::XactionRep::status() const
{
    static MemBuf buf;
    buf.reset();

    buf.append(" [", 2);

    if (makingVb)
        buf.Printf("M%d", static_cast<int>(makingVb));

    const BodyPipePointer &vp = theVirginRep.raw().body_pipe;
    if (!vp)
        buf.append(" !V", 3);
    else
    if (vp->stillConsuming(const_cast<XactionRep*>(this)))
        buf.append(" Vc", 3);
    else
        buf.append(" V?", 3);

    if (vbProductionFinished)
        buf.append(".", 1);


    buf.Printf(" A%d", static_cast<int>(proxyingAb));

    if (proxyingAb == opOn) {
        MessageRep *rep = dynamic_cast<MessageRep*>(theAnswerRep.get());
        Must(rep);
        const BodyPipePointer &ap = rep->raw().body_pipe;
        if (!ap)
            buf.append(" !A", 3);
        else if (ap->stillProducing(const_cast<XactionRep*>(this)))
            buf.append(" Ap", 3);
        else
            buf.append(" A?", 3);
    }

    buf.Printf(" %s%u]", id.Prefix, id.value);

    buf.terminate();

    return buf.content();
}
