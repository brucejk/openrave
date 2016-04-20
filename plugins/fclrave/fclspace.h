#ifndef OPENRAVE_FCL_SPACE
#define OPENRAVE_FCL_SPACE

/* TODO : - try using sets instead of vectors
          - exclude the attached objects in the link against env case
 */


// TODO : I should put these in some namespace...

typedef KinBody::LinkConstPtr LinkConstPtr;
typedef std::pair<LinkConstPtr, LinkConstPtr> LinkPair;
typedef boost::shared_ptr<fcl::BroadPhaseCollisionManager> BroadPhaseCollisionManagerPtr;
// Warning : this is the only place where we use std::shared_ptr (compatibility with fcl)
typedef shared_ptr<fcl::CollisionGeometry> CollisionGeometryPtr;
typedef boost::function<CollisionGeometryPtr (std::vector<fcl::Vec3f> const &points, std::vector<fcl::Triangle> const &triangles) > MeshFactory;
typedef std::vector<fcl::CollisionObject *> CollisionGroup;
typedef boost::shared_ptr<CollisionGroup> CollisionGroupPtr;
using OpenRAVE::ORE_Assert;

/* Helper functions for conversions from OpenRAVE to FCL */

Vector ConvertVectorFromFCL(fcl::Vec3f const &v)
{
    return Vector(v[0], v[1], v[2]);
}

fcl::Vec3f ConvertVectorToFCL(Vector const &v)
{
    return fcl::Vec3f(v.x, v.y, v.z);
}
fcl::Quaternion3f ConvertQuaternionToFCL(Vector const &v)
{
    return fcl::Quaternion3f(v[0], v[1], v[2], v[3]);
}

template <class T>
CollisionGeometryPtr ConvertMeshToFCL(std::vector<fcl::Vec3f> const &points,std::vector<fcl::Triangle> const &triangles)
{
    shared_ptr< fcl::BVHModel<T> > const model = make_shared<fcl::BVHModel<T> >();
    model->beginModel(triangles.size(), points.size());
    model->addSubModel(points, triangles);
    model->endModel();
    return model;
}


inline void UnregisterObjects(BroadPhaseCollisionManagerPtr manager, CollisionGroup& group)
{
    FOREACH(itcollobj, group) {
        manager->unregisterObject(*itcollobj);
    }
}


class FCLSpace : public boost::enable_shared_from_this<FCLSpace>
{
  
  
public:
    typedef boost::shared_ptr<fcl::CollisionObject> CollisionObjectPtr;
    typedef boost::shared_ptr<Transform> TransformPtr;
    // TODO : is it okay to leave a plain transform there instead of a pointer
    typedef pair<Transform, CollisionObjectPtr> TransformCollisionPair;



    inline boost::weak_ptr<FCLSpace> weak_space() {
        return shared_from_this();
    }



    // corresponds to FCLUserData
    class KinBodyInfo : public boost::enable_shared_from_this<KinBodyInfo>, public OpenRAVE::UserData
    {
public:


        struct LINK
        {
            LINK(KinBody::LinkPtr plink) : _plink(plink) {
            }

            virtual ~LINK() {
                Reset();
            }

            void Reset() {
                if( !!_linkManager ) {
                    _linkManager->clear();
                    _linkManager.reset();
                }
                FOREACH(itgeompair, vgeoms) {
                    if( !!_bodyManager ) {
                        _bodyManager->unregisterObject((*itgeompair).second.get());
                    }
		    if( !!_envManager ) {
                        _envManager->unregisterObject((*itgeompair).second.get());
                    }
                    (*itgeompair).second.reset();
                }
                vgeoms.resize(0);
            }

            KinBody::LinkPtr GetLink() {
                return _plink.lock();
            }

            KinBody::LinkWeakPtr _plink;

          // TODO : What about a dynamic collection of managers containing this link ?
	  BroadPhaseCollisionManagerPtr _envManager, _bodyManager, _linkManager;
            std::vector<TransformCollisionPair> vgeoms; // fcl variant of ODE dBodyID
            std::string bodylinkname; // for debugging purposes
        };

        KinBodyInfo() : nLastStamp(0)
        {
        }

        virtual ~KinBodyInfo() {
            Reset();
        }

        void Reset()
        {
            if( !!_bodyManager ) {
                _bodyManager->clear();
                _bodyManager.reset();
            }
            FOREACH(itlink, vlinks) {
                (*itlink)->Reset();
            }
            vlinks.resize(0);
            _geometrycallback.reset();
            // should-I reinitialize nLastStamp ?
        }

        // TODO : Is this used
        KinBodyPtr GetBody()
        {
            return _pbody.lock();
        }

        KinBodyWeakPtr _pbody;  // could we make this const ?
        int nLastStamp;  // used during synchronization ("is transform up to date")
        vector< boost::shared_ptr<LINK> > vlinks;
        BroadPhaseCollisionManagerPtr _bodyManager;
        OpenRAVE::UserDataPtr _geometrycallback;
    };

    typedef boost::shared_ptr<KinBodyInfo> KinBodyInfoPtr;
    typedef boost::shared_ptr<KinBodyInfo const> KinBodyInfoConstPtr;
    typedef boost::function<void (KinBodyInfoPtr)> SynchronizeCallbackFn;

    FCLSpace(EnvironmentBasePtr penv, const std::string& userdatakey)
        : _penv(penv), _userdatakey(userdatakey)
    {
        _manager = _CreateManagerFromBroadphaseAlgorithm("Naive");
        // TODO : test best default choice
        SetBVHRepresentation("OBB");
        // Naive : initialization working
        // SaP : not working infinite loop at line 352 of Broadphase_SaP.cpp
        // SSaP : initialization working
        // IntervalTree : not working received SIGSEV at line 427 of interval_tree.cpp
        // DynamicAABBTree : initialization working
        // DynamicAABBTree_Array : initialization working
        SetBroadphaseAlgorithm("DynamicAABBTree");
    }

    virtual ~FCLSpace()
    {
        DestroyEnvironment();
    }

    void DestroyEnvironment()
    {
        RAVELOG_VERBOSE("destroying ode collision environment\n");
        _manager->clear();
        FOREACH(itbody, _setInitializedBodies) {
            KinBodyInfoPtr pinfo = GetInfo(*itbody);
            if( !!pinfo ) {
                pinfo->Reset();
            }
            bool is_consistent = (*itbody)->RemoveUserData(_userdatakey);
            if( !is_consistent ) {
                RAVELOG_WARN("inconsistency detected with fclspace user data\n");
            }
        }
        _setInitializedBodies.clear();
    }


    KinBodyInfoPtr InitKinBody(KinBodyConstPtr pbody, KinBodyInfoPtr pinfo = KinBodyInfoPtr())
    {

        if( !pinfo ) {
            pinfo.reset(new KinBodyInfo());
        }

        pinfo->Reset();
        pinfo->_pbody = boost::const_pointer_cast<KinBody>(pbody);
        pinfo->_bodyManager = CreateManager();

        pinfo->vlinks.reserve(pbody->GetLinks().size());
        FOREACHC(itlink, pbody->GetLinks()) {

            boost::shared_ptr<KinBodyInfo::LINK> link(new KinBodyInfo::LINK(*itlink));

            pinfo->vlinks.push_back(link);
            link->_linkManager = CreateManager();
	    link->_bodyManager = pinfo->_bodyManager;
	    link->_envManager = _manager;
            link->bodylinkname = pbody->GetName() + "/" + (*itlink)->GetName();


            if(_geometrygroup.size() > 0 && (*itlink)->GetGroupNumGeometries(_geometrygroup) >= 0) {
                const std::vector<KinBody::GeometryInfoPtr>& vgeometryinfos = (*itlink)->GetGeometriesFromGroup(_geometrygroup);
                FOREACHC(itgeominfo, vgeometryinfos) {
                    const CollisionGeometryPtr pfclgeom = _CreateFCLGeomFromGeometryInfo(_meshFactory, **itgeominfo);
                    if( !pfclgeom ) {
                        continue;
                    }

                    // We do not set the transformation here and leave it to _Synchronize
                    CollisionObjectPtr pfclcoll = boost::make_shared<fcl::CollisionObject>(pfclgeom);
                    pfclcoll->setUserData(link.get());

                    link->vgeoms.push_back(TransformCollisionPair((*itgeominfo)->_t, pfclcoll));
                    link->_linkManager->registerObject(pfclcoll.get());
                    pinfo->_bodyManager->registerObject(pfclcoll.get());
                    _manager->registerObject(pfclcoll.get());
                }
            } else {
                FOREACHC(itgeom, (*itlink)->GetGeometries()) {
                    KinBody::GeometryInfo geominfo = (*itgeom)->GetInfo();
                    const CollisionGeometryPtr pfclgeom = _CreateFCLGeomFromGeometryInfo(_meshFactory, geominfo);
                    if( !pfclgeom ) {
                        continue;
                    }

                    // We do not set the transformation here and leave it to _Synchronize
                    CollisionObjectPtr pfclcoll = boost::make_shared<fcl::CollisionObject>(pfclgeom);
                    pfclcoll->setUserData(link.get());

                    link->vgeoms.push_back(TransformCollisionPair(geominfo._t, pfclcoll));
                    link->_linkManager->registerObject(pfclcoll.get());
                    pinfo->_bodyManager->registerObject(pfclcoll.get());
                    _manager->registerObject(pfclcoll.get());
                }
            }
        }

        pinfo->_geometrycallback = pbody->RegisterChangeCallback(KinBody::Prop_LinkGeometry, boost::bind(&FCLSpace::_ResetKinBodyCallback,boost::bind(&OpenRAVE::utils::sptr_from<FCLSpace>, weak_space()),boost::weak_ptr<KinBody const>(pbody)));

        pbody->SetUserData(_userdatakey, pinfo);
        _setInitializedBodies.insert(pbody);

        // make sure that synchronization do occur !
        pinfo->nLastStamp = pbody->GetUpdateStamp() - 1;
        _Synchronize(pinfo);


        return pinfo;
    }

    void SetGeometryGroup(const std::string& groupname)
    {
        if(groupname != _geometrygroup) {
            _geometrygroup = groupname;

            FOREACHC(itbody, _setInitializedBodies) {
                KinBodyInfoPtr pinfo = this->GetInfo(*itbody);
                if( !!pinfo ) {
                    InitKinBody(*itbody, pinfo);
                }
            }
        }
    }

    const std::string& GetGeometryGroup() const
    {
        return _geometrygroup;
    }


    // Already existing geometry not updated !
    // Is that the behaviour we want ?
    void SetBVHRepresentation(std::string const &type)
    {
        if (type == "AABB") {
            _meshFactory = &ConvertMeshToFCL<fcl::AABB>;
        } else if (type == "OBB") {
            _meshFactory = &ConvertMeshToFCL<fcl::OBB>;
        } else if (type == "RSS") {
            _meshFactory = &ConvertMeshToFCL<fcl::RSS>;
        } else if (type == "OBBRSS") {
            _meshFactory = &ConvertMeshToFCL<fcl::OBBRSS>;
        } else if (type == "kDOP16") {
            _meshFactory = &ConvertMeshToFCL< fcl::KDOP<16> >;
        } else if (type == "kDOP18") {
            _meshFactory = &ConvertMeshToFCL< fcl::KDOP<18> >;
        } else if (type == "kDOP24") {
            _meshFactory = &ConvertMeshToFCL< fcl::KDOP<24> >;
        } else if (type == "kIOS") {
            _meshFactory = &ConvertMeshToFCL<fcl::kIOS>;
        } else {
            RAVELOG_WARN(str(boost::format("Unknown BVH representation '%s'.") % type));
        }
    }

    void SetBroadphaseAlgorithm(std::string const &algorithm)
    {
        if(_broadPhaseCollisionManagerAlgorithm == algorithm) {
            return;
        }
        _broadPhaseCollisionManagerAlgorithm = algorithm;
        _manager = _CreateNewBroadPhaseCollisionManager(_manager);

        FOREACH(itbody, _setInitializedBodies) {
            KinBodyInfoPtr pinfo = GetInfo(*itbody);
            BOOST_ASSERT( !!pinfo );
            pinfo->_bodyManager = _CreateNewBroadPhaseCollisionManager(pinfo->_bodyManager);
            FOREACH(itlink, pinfo->vlinks) {
	      (*itlink)->_linkManager = _CreateNewBroadPhaseCollisionManager((*itlink)->_linkManager);
            }
        }
    }

    BroadPhaseCollisionManagerPtr _CreateNewBroadPhaseCollisionManager(BroadPhaseCollisionManagerPtr oldmanager) {
        CollisionGroup vcollObjects;
        oldmanager->getObjects(vcollObjects);
        BroadPhaseCollisionManagerPtr manager = _CreateManagerFromBroadphaseAlgorithm(_broadPhaseCollisionManagerAlgorithm);
        manager->registerObjects(vcollObjects);
	return manager;
    }

    void RemoveUserData(KinBodyConstPtr pbody)
    {
        if( !!pbody ) {
            _setInitializedBodies.erase(pbody);
            KinBodyInfoPtr pinfo = GetInfo(pbody);
            if( !!pinfo ) {
                pinfo->Reset();
            }
            bool is_consistent = pbody->RemoveUserData(_userdatakey);
            if( !is_consistent ) {
                RAVELOG_WARN("inconsistency detected with fclspace user data\n");
            }
        }
    }


    void Synchronize()
    {
        // We synchronize only the initialized bodies, which differs from oderave
        FOREACH(itbody, _setInitializedBodies) {
            Synchronize(*itbody);
        }
    }

    void Synchronize(KinBodyConstPtr pbody)
    {
        KinBodyInfoPtr pinfo = GetCreateInfo(pbody).first;
        BOOST_ASSERT( pinfo->GetBody() == pbody);
        _Synchronize(pinfo);
    }

    void SetSynchronizationCallback(const SynchronizeCallbackFn& synccallback) {
        _synccallback = synccallback;
    }

    KinBodyInfoPtr GetInfo(KinBodyConstPtr pbody)
    {
        return boost::dynamic_pointer_cast<KinBodyInfo>(pbody->GetUserData(_userdatakey));
    }

    std::pair<KinBodyInfoPtr, bool> GetCreateInfo(KinBodyConstPtr pbody)
    {
        KinBodyInfoPtr pinfo = this->GetInfo(pbody);
        bool bcreated = false;
        if( !pinfo ) {
            pinfo = InitKinBody(pbody, KinBodyInfoPtr());
            pbody->SetUserData(_userdatakey, pinfo);
            bcreated = true;
        }
        return std::make_pair(pinfo, bcreated);
    }


    void GetCollisionObjects(KinBodyConstPtr pbody, CollisionGroup& group)
    {
        KinBodyInfoPtr pinfo = GetInfo(pbody);
        BOOST_ASSERT( pinfo->GetBody() == pbody );
        CollisionGroup tmpGroup;
        pinfo->_bodyManager->getObjects(tmpGroup);
        copy(tmpGroup.begin(), tmpGroup.end(), back_inserter(group));
    }

    void GetCollisionObjects(LinkConstPtr plink, CollisionGroup &group)
    {
        // TODO : This is just so stupid, I should be able to reuse _linkManager efficiently !
        CollisionGroup tmpGroup;
        GetLinkManager(plink)->getObjects(tmpGroup);
        copy(tmpGroup.begin(), tmpGroup.end(), back_inserter(group));
    }


    class TemporaryManager {
public:
        virtual void Register(KinBodyConstPtr pbody) = 0;
        virtual void Register(LinkConstPtr plink) = 0;
        virtual BroadPhaseCollisionManagerPtr GetManager() = 0;
    };

    class BorrowManager : public TemporaryManager {
public:
        BorrowManager(boost::shared_ptr<FCLSpace> pfclspace) : _pfclspace(pfclspace)
        {
            _manager = _pfclspace->CreateManager();
        }

        ~BorrowManager() {
            CollisionGroup borrowedObjects;
            _manager->getObjects(borrowedObjects);
            _pfclspace->GetEnvManager()->registerObjects(borrowedObjects);
        }

        void Register(KinBodyConstPtr pbody) {
            KinBodyInfoPtr pinfo = _pfclspace->GetInfo(pbody);
            BOOST_ASSERT( pinfo->GetBody() == pbody );

            CollisionGroup tmpGroup;
            pinfo->_bodyManager->getObjects(tmpGroup);
            _manager->registerObjects(tmpGroup);
            UnregisterObjects(_pfclspace->GetEnvManager(), tmpGroup);
        }

        void Register(LinkConstPtr plink) {
            CollisionGroup tmpGroup;
            _pfclspace->GetLinkManager(plink)->getObjects(tmpGroup);
            _manager->registerObjects(tmpGroup);
            UnregisterObjects(_pfclspace->GetEnvManager(), tmpGroup);
        }

        BroadPhaseCollisionManagerPtr GetManager() {
            return _manager;
        }

private:
        BroadPhaseCollisionManagerPtr _manager;
        boost::shared_ptr<FCLSpace> _pfclspace;
    };

    class WrapperManager : public TemporaryManager {
public:
        WrapperManager(boost::shared_ptr<FCLSpace> pfclspace) : _pfclspace(pfclspace) {
            _manager = _pfclspace->CreateManager();
        }

        void Register(KinBodyConstPtr pbody) {
            KinBodyInfoPtr pinfo = _pfclspace->GetInfo(pbody);
            BOOST_ASSERT( pinfo->GetBody() == pbody );

            CollisionGroup tmpGroup;
            pinfo->_bodyManager->getObjects(tmpGroup);
            _manager->registerObjects(tmpGroup);
        }

        void Register(LinkConstPtr plink) {
            CollisionGroup tmpGroup;
            _pfclspace->GetLinkManager(plink)->getObjects(tmpGroup);
            _manager->registerObjects(tmpGroup);
        }

        BroadPhaseCollisionManagerPtr GetManager() {
            return _manager;
        }

private:
        BroadPhaseCollisionManagerPtr _manager;
        boost::shared_ptr<FCLSpace> _pfclspace;
    };

    class ExclusionManager : public TemporaryManager {

        ExclusionManager(boost::shared_ptr<FCLSpace> pfclspace) : _pfclspace(pfclspace) {
        }

        ~ExclusionManager() {
            _pfclspace->GetEnvManager()->registerObjects(vexcluded);
        }

        void Register(KinBodyConstPtr pbody) {
            KinBodyInfoPtr pinfo = _pfclspace->GetInfo(pbody);
            BOOST_ASSERT( pinfo->GetBody() == pbody );

            CollisionGroup tmpGroup;
            pinfo->_bodyManager->getObjects(tmpGroup);
            copy(tmpGroup.begin(), tmpGroup.end(), back_inserter(vexcluded));
            UnregisterObjects(_pfclspace->GetEnvManager(), tmpGroup);
        }

        void Register(LinkConstPtr plink) {
            CollisionGroup tmpGroup;
            _pfclspace->GetLinkManager(plink)->getObjects(tmpGroup);
            copy(tmpGroup.begin(), tmpGroup.end(), back_inserter(vexcluded));
            UnregisterObjects(_pfclspace->GetEnvManager(), tmpGroup);
        }

        BroadPhaseCollisionManagerPtr GetManager() {
            return BroadPhaseCollisionManagerPtr();
        }
private:
        CollisionGroup vexcluded;
        boost::shared_ptr<FCLSpace> _pfclspace;
    };

    typedef boost::shared_ptr<TemporaryManager> TemporaryManagerPtr;

    BroadPhaseCollisionManagerPtr GetEnvManager() const {
        return _manager;
    }

    BroadPhaseCollisionManagerPtr GetKinBodyManager(KinBodyConstPtr pbody) {
        KinBodyInfoPtr pinfo = GetInfo(pbody);
        return pinfo->_bodyManager;
    }

    BroadPhaseCollisionManagerPtr GetLinkManager(LinkConstPtr plink) {
        return GetLinkManager(plink->GetParent(), plink->GetIndex());
    }

    BroadPhaseCollisionManagerPtr GetLinkManager(KinBodyConstPtr pbody, int index) {
        KinBodyInfoPtr pinfo = GetInfo(pbody);
        if( !!pinfo ) {
            return pinfo->vlinks[index]->_linkManager;
        } else {
            return BroadPhaseCollisionManagerPtr();
        }
    }

    TemporaryManagerPtr CreateBorrowManager() {
        return boost::make_shared<BorrowManager>(shared_from_this());
    }

    TemporaryManagerPtr CreateTemporaryManager() {
        return boost::make_shared<WrapperManager>(shared_from_this());
    }

    TemporaryManagerPtr CreateExclusionManager() {
        return boost::make_shared<BorrowManager>(shared_from_this());
    }

    BroadPhaseCollisionManagerPtr CreateManager() {
        return _CreateManagerFromBroadphaseAlgorithm(_broadPhaseCollisionManagerAlgorithm);
    }




private:

    // couldn't we make this static / what about the tests on non-zero size (eg. box extents) ?
    static CollisionGeometryPtr _CreateFCLGeomFromGeometryInfo(const MeshFactory &mesh_factory, const KinBody::GeometryInfo &info)
    {
        switch(info._type) {

        case OpenRAVE::GT_None:
            return CollisionGeometryPtr();

        case OpenRAVE::GT_Box:
            return make_shared<fcl::Box>(info._vGeomData.x*2.0f,info._vGeomData.y*2.0f,info._vGeomData.z*2.0f);

        case OpenRAVE::GT_Sphere:
            return make_shared<fcl::Sphere>(info._vGeomData.x);

        case OpenRAVE::GT_Cylinder:
	  // TODO double check this
            return make_shared<fcl::Cylinder>(info._vGeomData.x, info._vGeomData.y);

        case OpenRAVE::GT_TriMesh:
        {
            const OpenRAVE::TriMesh& mesh = info._meshcollision;
            if (mesh.vertices.empty() || mesh.indices.empty()) {
                return CollisionGeometryPtr();
            }

            OPENRAVE_ASSERT_OP(mesh.indices.size() % 3, ==, 0);
            size_t const num_points = mesh.vertices.size();
            size_t const num_triangles = mesh.indices.size() / 3;

            std::vector<fcl::Vec3f> fcl_points(num_points);
            for (size_t ipoint = 0; ipoint < num_points; ++ipoint) {
                Vector v = mesh.vertices[ipoint];
                fcl_points[ipoint] = fcl::Vec3f(v.x, v.y, v.z);
            }

            std::vector<fcl::Triangle> fcl_triangles(num_triangles);
            for (size_t itri = 0; itri < num_triangles; ++itri) {
                int const *const tri_indices = &mesh.indices[3 * itri];
                fcl_triangles[itri] = fcl::Triangle(tri_indices[0], tri_indices[1], tri_indices[2]);
            }

            return mesh_factory(fcl_points, fcl_triangles);
        }

        // TODO : GT_Containers

        default:
            RAVELOG_WARN(str(boost::format("FCL doesn't support geom type %d")%info._type));
            return CollisionGeometryPtr();
        }
    }

    void _Synchronize(KinBodyInfoPtr pinfo)
    {
        KinBodyPtr pbody = pinfo->GetBody();
        if( pinfo->nLastStamp != pbody->GetUpdateStamp()) {
            vector<Transform> vtrans;
            pbody->GetLinkTransformations(vtrans);
            pinfo->nLastStamp = pbody->GetUpdateStamp();
            BOOST_ASSERT( pbody->GetLinks().size() == pinfo->vlinks.size() );
            BOOST_ASSERT( vtrans.size() == pinfo->vlinks.size() );
            for(size_t i = 0; i < vtrans.size(); ++i) {
                FOREACHC(itgeomcoll, pinfo->vlinks[i]->vgeoms) {
                    CollisionObjectPtr collObj = (*itgeomcoll).second;
                    Transform pose = vtrans[i] * (*itgeomcoll).first;
                    fcl::Vec3f newPosition = ConvertVectorToFCL(pose.trans);
                    fcl::Quaternion3f newOrientation = ConvertQuaternionToFCL(pose.rot);

                    collObj->setTranslation(newPosition);
                    collObj->setQuatRotation(newOrientation);
                    collObj->computeAABB();

                    pinfo->vlinks[i]->_linkManager->update(collObj.get());
                    pinfo->_bodyManager->update(collObj.get());
                    _manager->update(collObj.get());
                }
            }

            if( !!_synccallback ) {
                _synccallback(pinfo);
            }
        }
    }


    void _ResetKinBodyCallback(boost::weak_ptr<KinBody const> _pbody)
    {
        KinBodyConstPtr pbody(_pbody);
        std::pair<KinBodyInfoPtr, bool> infocreated = GetCreateInfo(pbody);
        if( !infocreated.second ) {
            BOOST_ASSERT( infocreated.first->GetBody() == pbody );
            InitKinBody(pbody, infocreated.first);
        }
    }

    static BroadPhaseCollisionManagerPtr _CreateManagerFromBroadphaseAlgorithm(std::string const &algorithm)
    {
        if(algorithm == "Naive") {
            return boost::make_shared<fcl::NaiveCollisionManager>();
        } else if(algorithm == "SaP") {
            return boost::make_shared<fcl::SaPCollisionManager>();
        } else if(algorithm == "SSaP") {
            return boost::make_shared<fcl::SSaPCollisionManager>();
            // It seems that some informations on the scene are necessaries
            //} else if(algorithm == "SpatialHash") {
            //return boost::make_shared<fcl::SpatialHashingCollisionManager>();
        } else if(algorithm == "IntervalTree") {
            return boost::make_shared<fcl::IntervalTreeCollisionManager>();
        } else if(algorithm == "DynamicAABBTree") {
            return boost::make_shared<fcl::DynamicAABBTreeCollisionManager>();
        } else if(algorithm == "DynamicAABBTree_Array") {
            return boost::make_shared<fcl::DynamicAABBTreeCollisionManager_Array>();
        } else {
	  throw OPENRAVE_EXCEPTION_FORMAT("Unknown broad-phase algorithm '%s'.", algorithm, OpenRAVE::ORE_InvalidArguments);
        }
    }


    EnvironmentBasePtr _penv;
    std::string _userdatakey;
    std::string _geometrygroup;
    SynchronizeCallbackFn _synccallback;

    std::string _broadPhaseCollisionManagerAlgorithm;
    BroadPhaseCollisionManagerPtr _manager;
    MeshFactory _meshFactory;

    std::set<KinBodyConstPtr> _setInitializedBodies;

};




#ifdef RAVE_REGISTER_BOOST
#include BOOST_TYPEOF_REGISTRATION_GROUP()
BOOST_TYPEOF_REGISTER_TYPE(FCLSpace)
BOOST_TYPEOF_REGISTER_TYPE(FCLSpace::KinBodyInfo)
BOOST_TYPEOF_REGISTER_TYPE(FCLSpace::KinBodyInfo::LINK)
#endif


#endif
