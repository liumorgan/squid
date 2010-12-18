/*
 * DEBUG: section 93    eCAP Interface
 */
#include "squid.h"
#include <list>
#include <libecap/adapter/service.h>
#include <libecap/common/config.h>
#include <libecap/common/name.h>
#include <libecap/common/named_values.h>
#include "adaptation/ecap/Config.h"
#include "adaptation/ecap/ServiceRep.h"
#include "adaptation/ecap/XactionRep.h"
#include "base/TextException.h"

// configured eCAP service wrappers
static std::list<Adaptation::Ecap::ServiceRep::AdapterService> TheServices;

/// wraps Adaptation::Ecap::ServiceConfig to allow eCAP visitors
class ConfigRep: public libecap::Config
{
public:
    typedef Adaptation::Ecap::ServiceConfig Master;
    typedef libecap::Name Name;
    typedef libecap::Area Area;

    ConfigRep(const Master &aMaster): master(aMaster) {}

    // libecap::Config API
    virtual void visitEach(libecap::NamedValueVisitor &visitor) const;

    const Master &master; ///< the configuration being wrapped
};

void
ConfigRep::visitEach(libecap::NamedValueVisitor &visitor) const
{
    // we may supply the params we know about too, but only if we set host ID
    static const Name optBypass("bypassable");
    if (!optBypass.assignedHostId())
         optBypass.assignHostId(1); // allows adapter to safely ignore this
    visitor.visit(optBypass, Area(master.bypass ? "1" : "0", 1));

    // visit adapter-specific options (i.e., those not recognized by Squid)
    typedef Master::Extensions::const_iterator MECI;
    for (MECI i = master.extensions.begin(); i != master.extensions.end(); ++i)
        visitor.visit(Name(i->first), Area::FromTempString(i->second));
}



Adaptation::Ecap::ServiceRep::ServiceRep(ServiceConfigPointer cfg):
        /*AsyncJob("Adaptation::Ecap::ServiceRep"),*/ Adaptation::Service(cfg),
        isDetached(false)
{
}

Adaptation::Ecap::ServiceRep::~ServiceRep()
{
}

void Adaptation::Ecap::ServiceRep::noteFailure()
{
    assert(false); // XXX: should this be ICAP-specific?
}

void
Adaptation::Ecap::ServiceRep::finalize()
{
    Adaptation::Service::finalize();
    theService = FindAdapterService(cfg().uri);
    if (theService) {
        debugs(93,3, HERE << "configuring eCAP service: " << theService->uri());
        ConfigRep cfgRep(dynamic_cast<const ServiceConfig&>(cfg()));
        theService->configure(cfgRep);

        debugs(93,3, HERE << "starting eCAP service: " << theService->uri());
        theService->start();
    } else {
        debugs(93,1, "Warning: configured ecap_service was not loaded: " <<
               cfg().uri);
    }
}

bool Adaptation::Ecap::ServiceRep::probed() const
{
    return true; // we "probe" the adapter in finalize().
}

bool Adaptation::Ecap::ServiceRep::up() const
{
    return theService != NULL;
}

bool Adaptation::Ecap::ServiceRep::wantsUrl(const String &urlPath) const
{
    Must(up());
    return theService->wantsUrl(urlPath.termedBuf());
}

Adaptation::Initiate *
Adaptation::Ecap::ServiceRep::makeXactLauncher(HttpMsg *virgin,
        HttpRequest *cause)
{
    Must(up());
    XactionRep *rep = new XactionRep(virgin, cause, Pointer(this));
    XactionRep::AdapterXaction x(theService->makeXaction(rep));
    rep->master(x);
    return rep;
}

// returns a temporary string depicting service status, for debugging
const char *Adaptation::Ecap::ServiceRep::status() const
{
    // TODO: move generic stuff from eCAP and ICAP to Adaptation
    static MemBuf buf;

    buf.reset();
    buf.append("[", 1);

    if (up())
        buf.append("up", 2);
    else
        buf.append("down", 4);

    if (detached())
        buf.append(",detached", 9);

    buf.append("]", 1);
    buf.terminate();

    return buf.content();
}

void Adaptation::Ecap::ServiceRep::detach()
{
    isDetached = true;
}

bool Adaptation::Ecap::ServiceRep::detached() const
{
    return isDetached;
}

Adaptation::Ecap::ServiceRep::AdapterService
Adaptation::Ecap::FindAdapterService(const String& serviceUri)
{
    typedef std::list<ServiceRep::AdapterService>::const_iterator ASCI;
    for (ASCI s = TheServices.begin(); s != TheServices.end(); ++s) {
        Must(*s);
        if (serviceUri == (*s)->uri().c_str())
            return *s;
    }
    return ServiceRep::AdapterService();
}

void
Adaptation::Ecap::RegisterAdapterService(const Adaptation::Ecap::ServiceRep::AdapterService& adapterService)
{
    typedef std::list<ServiceRep::AdapterService>::iterator ASI;
    for (ASI s = TheServices.begin(); s != TheServices.end(); ++s) {
        Must(*s);
        if (adapterService->uri() == (*s)->uri()) {
            *s = adapterService;
            debugs(93, 3, "updated eCAP module service: " <<
                   adapterService->uri());
            return;
        }
    }
    TheServices.push_back(adapterService);
    debugs(93, 3, "registered eCAP module service: " << adapterService->uri());
}

void
Adaptation::Ecap::UnregisterAdapterService(const String& serviceUri)
{
    typedef std::list<ServiceRep::AdapterService>::iterator ASI;
    for (ASI s = TheServices.begin(); s != TheServices.end(); ++s) {
        if (serviceUri == (*s)->uri().c_str()) {
            TheServices.erase(s);
            debugs(93, 3, "unregistered eCAP module service: " << serviceUri);
            return;
        }
    }
    debugs(93, 3, "failed to unregister eCAP module service: " << serviceUri);
}

void
Adaptation::Ecap::CheckUnusedAdapterServices(const Adaptation::Services& cfgs)
{
    typedef std::list<ServiceRep::AdapterService>::const_iterator ASCI;
    for (ASCI loaded = TheServices.begin(); loaded != TheServices.end();
            ++loaded) {
        bool found = false;
        for (Services::const_iterator cfged = cfgs.begin();
                cfged != cfgs.end() && !found; ++cfged) {
            found = (*cfged)->cfg().uri == (*loaded)->uri().c_str();
        }
        if (!found)
            debugs(93, 1, "Warning: loaded eCAP service has no matching " <<
                   "ecap_service config option: " << (*loaded)->uri());
    }
}
