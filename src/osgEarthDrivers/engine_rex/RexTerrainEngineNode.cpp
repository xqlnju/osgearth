/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
* Copyright 2008-2014 Pelican Mapping
* http://osgearth.org
*
* osgEarth is free software; you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>
*/
#include "RexTerrainEngineNode"
#include "Shaders"
#include "SelectionInfo"
#include "TerrainCuller"

#include <osgEarth/ImageUtils>
#include <osgEarth/Registry>
#include <osgEarth/Capabilities>
#include <osgEarth/VirtualProgram>
#include <osgEarth/MapModelChange>
#include <osgEarth/Progress>
#include <osgEarth/ShaderLoader>
#include <osgEarth/Utils>
#include <osgEarth/ObjectIndex>

#include <osg/Version>
#include <osg/BlendFunc>
#include <osg/Depth>
#include <osg/CullFace>

#include <cstdlib> // for getenv

#define LC "[RexTerrainEngineNode] "

using namespace osgEarth::Drivers::RexTerrainEngine;
using namespace osgEarth;

//------------------------------------------------------------------------

namespace
{
    // adapter that lets RexTerrainEngineNode listen to Map events
    struct RexTerrainEngineNodeMapCallbackProxy : public MapCallback
    {
        RexTerrainEngineNodeMapCallbackProxy(RexTerrainEngineNode* node) : _node(node) { }
        osg::observer_ptr<RexTerrainEngineNode> _node;

        void onMapInfoEstablished( const MapInfo& mapInfo ) {
            osg::ref_ptr<RexTerrainEngineNode> node;
            if ( _node.lock(node) )
                node->onMapInfoEstablished( mapInfo );
        }

        void onMapModelChanged( const MapModelChange& change ) {
            osg::ref_ptr<RexTerrainEngineNode> node;
            if ( _node.lock(node) )
                node->onMapModelChanged( change );
        }
    };
}

//---------------------------------------------------------------------------

static Threading::ReadWriteMutex s_engineNodeCacheMutex;
//Caches the engines that have been created
typedef std::map<UID, osg::observer_ptr<RexTerrainEngineNode> > EngineNodeCache;

static
EngineNodeCache& getEngineNodeCache()
{
    static EngineNodeCache s_cache;
    return s_cache;
}

void
RexTerrainEngineNode::registerEngine(RexTerrainEngineNode* engineNode)
{
    Threading::ScopedWriteLock exclusiveLock( s_engineNodeCacheMutex );
    getEngineNodeCache()[engineNode->_uid] = engineNode;
    OE_DEBUG << LC << "Registered engine " << engineNode->_uid << std::endl;
}

// since this method is called in a database pager thread, we use a ref_ptr output
// parameter to avoid the engine node being destructed between the time we 
// return it and the time it's accessed; this could happen if the user removed the
// MapNode from the scene during paging.
void
RexTerrainEngineNode::getEngineByUID( UID uid, osg::ref_ptr<RexTerrainEngineNode>& output )
{
    Threading::ScopedReadLock sharedLock( s_engineNodeCacheMutex );
    EngineNodeCache::const_iterator k = getEngineNodeCache().find( uid );
    if (k != getEngineNodeCache().end())
        output = k->second.get();
}

UID
RexTerrainEngineNode::getUID() const
{
    return _uid;
}

//------------------------------------------------------------------------

RexTerrainEngineNode::ElevationChangedCallback::ElevationChangedCallback( RexTerrainEngineNode* terrain ):
_terrain( terrain )
{
    //nop
}

void
RexTerrainEngineNode::ElevationChangedCallback::onVisibleChanged( TerrainLayer* layer )
{
    _terrain->refresh(true); // true => force a dirty
}

//------------------------------------------------------------------------

RexTerrainEngineNode::RexTerrainEngineNode() :
TerrainEngineNode     ( ),
_terrain              ( 0L ),
_update_mapf          ( 0L ),
_tileCount            ( 0 ),
_tileCreationTime     ( 0.0 ),
_batchUpdateInProgress( false ),
_refreshRequired      ( false ),
_stateUpdateRequired  ( false )
{
    // unique ID for this engine:
    _uid = Registry::instance()->createUID();

    // always require elevation.
    _requireElevationTextures = true;

    // install an elevation callback so we can update elevation data
    _elevationCallback = new ElevationChangedCallback( this );

    // static shaders.
    if ( Registry::capabilities().supportsGLSL() )
    {
        osg::StateSet* stateset = getOrCreateStateSet();
        VirtualProgram* vp = VirtualProgram::getOrCreate(stateset);
        Shaders package;
        package.load(vp, package.SDK);
    }

    // TODO: replace with a "renderer" object that can return statesets
    // for different layer types, or something.
    _imageLayerStateSet = new osg::StateSet();
}

RexTerrainEngineNode::~RexTerrainEngineNode()
{
    if ( _update_mapf )
    {
        delete _update_mapf;
    }
}

void
RexTerrainEngineNode::preInitialize( const Map* map, const TerrainOptions& options )
{
    // Force the mercator fast path off, since REX does not support it yet.
    TerrainOptions myOptions = options;
    myOptions.enableMercatorFastPath() = false;

    TerrainEngineNode::preInitialize( map, myOptions );
}

void
RexTerrainEngineNode::postInitialize( const Map* map, const TerrainOptions& options )
{
    // Force the mercator fast path off, since REX does not support it yet.
    TerrainOptions myOptions = options;
    myOptions.enableMercatorFastPath() = false;

    TerrainEngineNode::postInitialize( map, myOptions );

    // Initialize the map frames. We need one for the update thread and one for the
    // cull thread. Someday we can detect whether these are actually the same thread
    // (depends on the viewer's threading mode).
    _update_mapf = new MapFrame( map, Map::ENTIRE_MODEL );

    // merge in the custom options:
    _terrainOptions.merge( myOptions );

    // morphing imagery LODs requires we bind parent textures to their own unit.
    if ( _terrainOptions.morphImagery() == true )
    {
        _requireParentTextures = true;
    }

    // Terrain morphing doesn't work in projected maps:
    if (map->getSRS()->isProjected())
    {
        _terrainOptions.morphTerrain() = false;
    }

    // if the envvar for tile expiration is set, overide the options setting
    const char* val = ::getenv("OSGEARTH_EXPIRATION_THRESHOLD");
    if ( val )
    {
        _terrainOptions.expirationThreshold() = as<unsigned>(val, _terrainOptions.expirationThreshold().get());
        OE_INFO << LC << "Expiration threshold set by env var = " << _terrainOptions.expirationThreshold().get() << "\n";
    }

    // if the envvar for hires prioritization is set, override the options setting
    const char* hiresFirst = ::getenv("OSGEARTH_HIGH_RES_FIRST");
    if ( hiresFirst )
    {
        _terrainOptions.highResolutionFirst() = true;
    }

    // check for normal map generation (required for lighting).
    if ( _terrainOptions.normalMaps() == true )
    {
        this->_requireNormalTextures = true;
    }

    // A shared registry for tile nodes in the scene graph. Enable revision tracking
    // if requested in the options. Revision tracking lets the registry notify all
    // live tiles of the current map revision so they can inrementally update
    // themselves if necessary.
    _liveTiles = new TileNodeRegistry("live");
    _liveTiles->setMapRevision( _update_mapf->getRevision() );

    // A resource releaser that will call releaseGLObjects() on expired objects.
    _releaser = new ResourceReleaser();
    this->addChild(_releaser.get());

    // A shared geometry pool.
    _geometryPool = new GeometryPool( _terrainOptions );
    _geometryPool->setReleaser( _releaser.get());
    this->addChild( _geometryPool.get() );

    // Make a tile loader
    PagerLoader* loader = new PagerLoader( this );
    loader->setMergesPerFrame( _terrainOptions.mergesPerFrame().get() );
    _loader = loader;
    this->addChild( _loader.get() );

    // Make a tile unloader
    _unloader = new UnloaderGroup( _liveTiles.get() );
    _unloader->setThreshold( _terrainOptions.expirationThreshold().get() );
    _unloader->setReleaser(_releaser.get());
    this->addChild( _unloader.get() );
    
    // handle an already-established map profile:
    MapInfo mapInfo( map );
    if ( _update_mapf->getProfile() )
    {
        // NOTE: this will initialize the map with the startup layers
        onMapInfoEstablished( mapInfo );
    }

    // install a layer callback for processing further map actions:
    map->addMapCallback( new RexTerrainEngineNodeMapCallbackProxy(this) );

    // Prime with existing layers:
    _batchUpdateInProgress = true;

    ElevationLayerVector elevationLayers;
    map->getLayers( elevationLayers );
    for( ElevationLayerVector::const_iterator i = elevationLayers.begin(); i != elevationLayers.end(); ++i )
        addElevationLayer( i->get() );

    ImageLayerVector imageLayers;
    map->getLayers( imageLayers );
    for( ImageLayerVector::iterator i = imageLayers.begin(); i != imageLayers.end(); ++i )
        addTileLayer( i->get() );

    _batchUpdateInProgress = false;

    // set up the initial shaders
    updateState();

    // register this instance to the osgDB plugin can find it.
    registerEngine( this );

    // now that we have a map, set up to recompute the bounds
    dirtyBound();
}


osg::BoundingSphere
RexTerrainEngineNode::computeBound() const
{
    return TerrainEngineNode::computeBound();
}

void
RexTerrainEngineNode::invalidateRegion(const GeoExtent& extent,
                                       unsigned         minLevel,
                                       unsigned         maxLevel)
{
    if ( _liveTiles.valid() )
    {
        GeoExtent extentLocal = extent;

        if ( !extent.getSRS()->isEquivalentTo(this->getMap()->getSRS()) )
        {
            extent.transform(this->getMap()->getSRS(), extentLocal);
        }
        
        _liveTiles->setDirty(extentLocal, minLevel, maxLevel);
    }
}

void
RexTerrainEngineNode::refresh(bool forceDirty)
{
    if ( _batchUpdateInProgress )
    {
        _refreshRequired = true;
    }
    else
    {
        dirtyTerrain();

        _refreshRequired = false;
    }
}

void
RexTerrainEngineNode::onMapInfoEstablished( const MapInfo& mapInfo )
{
    dirtyTerrain();
}

osg::StateSet*
RexTerrainEngineNode::getSurfaceStateSet()
{
    return _imageLayerStateSet.get();
}

void
RexTerrainEngineNode::setupRenderBindings()
{
    // "SHARED" is the start of shared layers, so we always want the bindings
    // vector to be at least that size.
    _renderBindings.resize(SamplerBinding::SHARED);

    SamplerBinding& color = _renderBindings[SamplerBinding::COLOR];
    color.usage()       = SamplerBinding::COLOR;
    color.samplerName() = "oe_layer_tex";
    color.matrixName()  = "oe_layer_texMatrix";
    getResources()->reserveTextureImageUnit( color.unit(), "Terrain Color" );
    
    SamplerBinding& elevation = _renderBindings[SamplerBinding::ELEVATION];
    elevation.usage()       = SamplerBinding::ELEVATION;
    elevation.samplerName() = "oe_tile_elevationTex";
    elevation.matrixName()  = "oe_tile_elevationTexMatrix";
    getResources()->reserveTextureImageUnit( elevation.unit(), "Terrain Elevation" );
    
    SamplerBinding& normal = _renderBindings[SamplerBinding::NORMAL];
    normal.usage()       = SamplerBinding::NORMAL;
    normal.samplerName() = "oe_tile_normalTex";
    normal.matrixName()  = "oe_tile_normalTexMatrix";
    getResources()->reserveTextureImageUnit( normal.unit(), "Terrain Normals" );
    
    SamplerBinding& colorParent = _renderBindings[SamplerBinding::COLOR_PARENT];
    colorParent.usage()       = SamplerBinding::COLOR_PARENT;
    colorParent.samplerName() = "oe_layer_texParent";
    colorParent.matrixName()  = "oe_layer_texParentMatrix";
    getResources()->reserveTextureImageUnit( colorParent.unit(), "Terrain Color (Parent)" );
}

void
RexTerrainEngineNode::dirtyTerrain()
{
    //TODO: scrub the geometry pool?

    // clear the loader:
    _loader->clear();

    if ( _terrain )
    {
        this->removeChild( _terrain );
    }

    // New terrain
    _terrain = new osg::Group();
    this->addChild( _terrain );

    // are we LOD blending?
    bool setupParentData = 
        _terrainOptions.morphImagery() == true || // gw: redundant?
        this->parentTexturesRequired();
    
    // reserve GPU unit for the main color texture:
    if ( _renderBindings.empty() )
    {
        setupRenderBindings();
    }

#if 0
    // Calculate the LOD morphing parameters:
    double averageRadius = 0.5*(
        _update_mapf->getMapInfo().getSRS()->getEllipsoid()->getRadiusEquator() +
        _update_mapf->getMapInfo().getSRS()->getEllipsoid()->getRadiusPolar());

    double farLOD = 
        _terrainOptions.minTileRangeFactor().get() *
        3.214 * averageRadius;

    _selectionInfo.initialize(
        0u, // always zero, not the terrain options firstLOD
        std::min( _terrainOptions.maxLOD().get(), 20u ), //19u ),
        _terrainOptions.tileSize().get(),
        farLOD );
#else
    // Calculate the LOD morphing parameters:
    _selectionInfo.initialize(
        0u, // always zero, not the terrain options firstLOD
        std::min( _terrainOptions.maxLOD().get(), 20u ), //19u ),
        _terrainOptions.tileSize().get(),
        _update_mapf->getMapInfo().getProfile(),        
        _terrainOptions.minTileRangeFactor().get() );
#endif

    // clear out the tile registry:
    if ( _liveTiles.valid() )
    {
        _liveTiles->releaseAll(_releaser.get());
    }

    // Factory to create the root keys:
    EngineContext* context = getEngineContext();

    // Build the first level of the terrain.
    // Collect the tile keys comprising the root tiles of the terrain.
    std::vector<TileKey> keys;
    _update_mapf->getProfile()->getAllKeysAtLOD( *_terrainOptions.firstLOD(), keys );

    // create a root node for each root tile key.
    OE_INFO << LC << "Creating " << keys.size() << " root keys.." << std::endl;

    unsigned child = 0;
    for( unsigned i=0; i<keys.size(); ++i )
    {
        TileNode* tileNode = new TileNode();
        if (context->getOptions().minExpiryFrames().isSet())
        {
            tileNode->setMinimumExpiryFrames( *context->getOptions().minExpiryFrames() );
        }
        if (context->getOptions().minExpiryTime().isSet())
        {         
            tileNode->setMinimumExpiryTime( *context->getOptions().minExpiryTime() );
        }
                
        // Next, build the surface geometry for the node.
        tileNode->create( keys[i], 0L, context );

        // Add it to the scene graph
        _terrain->addChild( tileNode );

        // And load the tile's data synchronously (only for root tiles).
        tileNode->loadSync( context );

    }

    updateState();

    // Call the base class
    TerrainEngineNode::dirtyTerrain();
}

namespace
{
    // debugging
    struct CheckForOrphans : public TileNodeRegistry::ConstOperation {
        void operator()( const TileNodeRegistry::TileNodeMap& tiles ) const {
            unsigned count = 0;
            for(TileNodeRegistry::TileNodeMap::const_iterator i = tiles.begin(); i != tiles.end(); ++i ) {
                if ( i->second.tile->referenceCount() == 1 ) {
                    count++;
                }
            }
            if ( count > 0 )
                OE_WARN << LC << "Oh no! " << count << " orphaned tiles in the reg" << std::endl;
        }
    };
}


void
RexTerrainEngineNode::dirtyState()
{
    // TODO: perhaps defer this until the next update traversal so we don't 
    // reinitialize the state multiple times unnecessarily. 
    updateState();
}


void
RexTerrainEngineNode::traverse(osg::NodeVisitor& nv)
{
    if ( nv.getVisitorType() == nv.CULL_VISITOR )
    {
        // Inform the registry of the current frame so that Tiles have access
        // to the information.
        if ( _liveTiles.valid() && nv.getFrameStamp() )
        {
            _liveTiles->setTraversalFrame( nv.getFrameStamp()->getFrameNumber() );
        }
    }

#if 0
    static int c = 0;
    if ( ++c % 60 == 0 )
    {
        OE_NOTICE << LC << "Live = " << _liveTiles->size() << ", Dead = " << _deadTiles->size() << std::endl;
        _liveTiles->run( CheckForOrphans() );
    }
#endif
    
    if ( nv.getVisitorType() == nv.CULL_VISITOR && _loader.valid() ) // ensures that postInitialize has run
    {
        VisitorData::store(nv, ENGINE_CONTEXT_TAG, this->getEngineContext());

        osgUtil::CullVisitor* cv = static_cast<osgUtil::CullVisitor*>(&nv);

        this->getEngineContext()->startCull( cv );
        
        TerrainCuller culler;
        culler.setFrameStamp(new osg::FrameStamp(*nv.getFrameStamp()));
        culler.setDatabaseRequestHandler(nv.getDatabaseRequestHandler());
        culler.pushReferenceViewPoint(cv->getReferenceViewPoint());
        culler.pushViewport(cv->getViewport());
        culler.pushProjectionMatrix(cv->getProjectionMatrix());
        culler.pushModelViewMatrix(cv->getModelViewMatrix(), cv->getCurrentCamera()->getReferenceFrame());
        culler._camera = cv->getCurrentCamera();
        culler._context = this->getEngineContext();
        culler.setup(*_update_mapf, this->getEngineContext()->getRenderBindings(), getSurfaceStateSet());

        // Assemble the terrain drawable:
        _terrain->accept(culler);

        // If we're using geometry pooling, optimize the drawable for shared state:
        if (getEngineContext()->getGeometryPool()->isEnabled())
        {
            culler._terrain.sortDrawCommands();
        }

        // The common stateset for the terrain:
        cv->pushStateSet(_terrain->getOrCreateStateSet());

        // Push all the layers to draw on to the cull visitor,
        // keeping track of render order.
        LayerDrawable* lastLayer = 0L;
        unsigned order = 0;
        bool surfaceStateSetPushed = false;

        for(LayerDrawableList::iterator i = culler._terrain.layers().begin();
            i != culler._terrain.layers().end();
            ++i)
        {
            if (!i->get()->_tiles.empty())
            {
                lastLayer = i->get();
                lastLayer->_order = order++;

                // if this is a RENDERTYPE_TILE, we need to activate the default surface state set.
                if (lastLayer->_layer && lastLayer->_layer->getRenderType() == Layer::RENDERTYPE_TILE)
                {
                    if (!surfaceStateSetPushed)
                        cv->pushStateSet(getSurfaceStateSet());
                    surfaceStateSetPushed = true;
                }
                else if (surfaceStateSetPushed)
                {
                    cv->popStateSet();
                    surfaceStateSetPushed = false;
                }                    

                cv->apply(*lastLayer);
            }
        }

        // The last layer to render must clear up the OSG state,
        // otherwise it will be corrupt and can lead to crashing.
        if (lastLayer)
        {
            lastLayer->_clearOsgState = true;
        }
                
        if (surfaceStateSetPushed)
        {
            cv->popStateSet();
            surfaceStateSetPushed = false;
        }

        // pop the common terrain state set
        cv->popStateSet();

        this->getEngineContext()->endCull( cv );

        // traverse all the other children (geometry pool, loader/unloader, etc.)
        for (unsigned i = 0; i<getNumChildren(); ++i)
        {
            if (getChild(i) != _terrain.get())
                getChild(i)->accept(nv);
        }
    }

    else
    {
        TerrainEngineNode::traverse( nv );
    }
}


EngineContext*
RexTerrainEngineNode::getEngineContext()
{
    osg::ref_ptr<EngineContext>& context = _perThreadTileGroupFactories.get(); // thread-safe get
    if ( !context.valid() )
    {
        // initialize a key node factory.
        context = new EngineContext(
            getMap(),
            this, // engine
            _geometryPool.get(),
            _loader.get(),
            _unloader.get(),
            _liveTiles.get(),
            _renderBindings,
            _terrainOptions,
            _selectionInfo,
            _tilePatchCallbacks);
    }

    return context.get();
}

unsigned int
RexTerrainEngineNode::computeSampleSize(unsigned int levelOfDetail)
{    
    unsigned maxLevel = std::min( *_terrainOptions.maxLOD(), 19u ); // beyond LOD 19 or 20, morphing starts to lose precision.
    unsigned int meshSize = *_terrainOptions.tileSize();

    unsigned int sampleSize = meshSize;
    int level = maxLevel; // make sure it's signed for the loop below to work

    while( level >= 0 && levelOfDetail != level)
    {
        sampleSize = sampleSize * 2 - 1;
        level--;
    }

    return sampleSize;    
}

osg::Vec3d getWorld( const GeoHeightField& geoHF, unsigned int c, unsigned int r)
{
    double x = geoHF.getExtent().xMin() + (double)c * geoHF.getXInterval();
    double y = geoHF.getExtent().yMin() + (double)r * geoHF.getYInterval();
    double h = geoHF.getHeightField()->getHeight(c,r);

    osg::Vec3d world;
    GeoPoint point(geoHF.getExtent().getSRS(), x, y, h );
    point.toWorld( world );    
    return world;
}

osg::Node* renderHeightField(const GeoHeightField& geoHF)
{
    osg::MatrixTransform* mt = new osg::MatrixTransform;

    GeoPoint centroid;
    geoHF.getExtent().getCentroid(centroid);

    osg::Matrix world2local, local2world;
    centroid.createWorldToLocal( world2local );
    local2world.invert( world2local );

    mt->setMatrix( local2world );

    osg::Geometry* geometry = new osg::Geometry;
    osg::Geode* geode = new osg::Geode;
    geode->addDrawable( geometry );
    mt->addChild( geode );

    osg::Vec3Array* verts = new osg::Vec3Array;
    geometry->setVertexArray( verts );

    for (unsigned int c = 0; c < geoHF.getHeightField()->getNumColumns() - 1; c++)
    {
        for (unsigned int r = 0; r < geoHF.getHeightField()->getNumRows() - 1; r++)
        {
            // Add two triangles 
            verts->push_back( getWorld( geoHF, c,     r    ) * world2local );
            verts->push_back( getWorld( geoHF, c + 1, r    ) * world2local );
            verts->push_back( getWorld( geoHF, c + 1, r + 1) * world2local );

            verts->push_back( getWorld( geoHF, c,     r    ) * world2local );
            verts->push_back( getWorld( geoHF, c + 1, r + 1) * world2local );
            verts->push_back( getWorld( geoHF, c,     r + 1) * world2local );
        }
    }
    geode->setCullingActive(false);
    mt->setCullingActive(false);

    geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, verts->size()));      

    osg::Vec4ubArray* colors = new osg::Vec4ubArray();
    colors->push_back(osg::Vec4ub(255,0,0,255));
    geometry->setColorArray(colors, osg::Array::BIND_OVERALL);
    mt->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
    mt->getOrCreateStateSet()->setRenderBinDetails(99, "RenderBin");        

    return mt;
}


osg::Node*
RexTerrainEngineNode::createTile( const TileKey& key )
{
     // Compute the sample size to use for the key's level of detail that will line up exactly with the tile size of the highest level of subdivision of the rex engine.
    unsigned int sampleSize = computeSampleSize( key.getLevelOfDetail() );    
    OE_INFO << LC << "Computed a sample size of " << sampleSize << " for lod " << key.getLevelOfDetail() << std::endl;

    TileKey sampleKey = key;

    // ALWAYS use 257x257 b/c that is what rex always uses.
    osg::ref_ptr< osg::HeightField > out_hf = HeightFieldUtils::createReferenceHeightField(
            key.getExtent(), 257, 257, true );

    sampleKey = key;

    bool populated = false;
    while (!populated)
    {
        populated = _update_mapf->populateHeightField(
            out_hf,
            sampleKey,
            true, // convertToHAE
            0 );

        if (!populated)
        {
            // Fallback on the parent
            sampleKey = sampleKey.createParentKey();
            if (!sampleKey.valid())
            {
                return 0;
            }
        }
    }

    // cannot happen (says coverity; see loop above), so commenting this out -gw
#if 0
    if (!populated)
    {
        // We have no heightfield so just create a reference heightfield.
        out_hf = HeightFieldUtils::createReferenceHeightField( key.getExtent(), 257, 257);
        sampleKey = key;
    }
#endif

    GeoHeightField geoHF( out_hf.get(), sampleKey.getExtent() );    
    if (sampleKey != key)
    {   
        geoHF = geoHF.createSubSample( key.getExtent(), sampleSize, sampleSize, osgEarth::INTERP_BILINEAR);         
    }

    // We should now have a heightfield that matches up exactly with the requested key at the appropriate resolution.
    // Turn it into triangles.
    return renderHeightField( geoHF );      
}


void
RexTerrainEngineNode::onMapModelChanged( const MapModelChange& change )
{
    if ( change.getAction() == MapModelChange::BEGIN_BATCH_UPDATE )
    {
        _batchUpdateInProgress = true;
    }

    else if ( change.getAction() == MapModelChange::END_BATCH_UPDATE )
    {
        _batchUpdateInProgress = false;

        if ( _refreshRequired )
            refresh();

        if ( _stateUpdateRequired )
            updateState();
    }

    else
    {
        // update the thread-safe map model copy:
        if ( _update_mapf->sync() )
        {
            _liveTiles->setMapRevision( _update_mapf->getRevision() );
        }

        // dispatch the change handler
        if ( change.getLayer() )
        {
            // then apply the actual change:
            switch( change.getAction() )
            {
            case MapModelChange::ADD_LAYER:
                if (change.getLayer()->getRenderType() == Layer::RENDERTYPE_TILE)
                    addTileLayer( change.getLayer() );
                else if (change.getElevationLayer())
                    addElevationLayer(change.getElevationLayer());
                break;

            case MapModelChange::REMOVE_LAYER:
                if (change.getImageLayer())
                    removeImageLayer( change.getImageLayer() );
                else if (change.getElevationLayer())
                    removeElevationLayer(change.getElevationLayer());
                break;

            case MapModelChange::TOGGLE_ELEVATION_LAYER:
                toggleElevationLayer( change.getElevationLayer() );
                break;

            default: 
                break;
            }
        }
    }
}

void
RexTerrainEngineNode::addTileLayer(Layer* tileLayer)
{
    if ( tileLayer && tileLayer->getEnabled() )
    {
        // Install the image layer stateset on this layer.
        // Later we will refactor this into an ImageLayerRenderer or something similar.
        //osg::StateSet* stateSet = tileLayer->getOrCreateStateSet();
        //stateSet->merge(*getSurfaceStateSet());

        ImageLayer* imageLayer = dynamic_cast<ImageLayer*>(tileLayer);
        if (imageLayer)
        {
            // for a shared layer, allocate a shared image unit if necessary.
            if ( imageLayer->isShared() )
            {
                optional<int>& unit = imageLayer->shareImageUnit();
                if ( !unit.isSet() )
                {
                    int temp;
                    if ( getResources()->reserveTextureImageUnit(temp) )
                    {
                        imageLayer->shareImageUnit() = temp;
                        OE_INFO << LC << "Image unit " << temp << " assigned to shared layer " << imageLayer->getName() << std::endl;
                    }
                    else
                    {
                        OE_WARN << LC << "Insufficient GPU image units to share layer " << imageLayer->getName() << std::endl;
                    }
                }

                // Build a sampler binding for the shared layer.
                if ( unit.isSet() )
                {
                    // Find the next empty SHARED slot:
                    unsigned newIndex = SamplerBinding::SHARED;
                    while (_renderBindings[newIndex].isActive())
                        ++newIndex;

                    // Put the new binding there:
                    SamplerBinding& newBinding = _renderBindings[newIndex];
                    newBinding.usage()       = SamplerBinding::SHARED;
                    newBinding.sourceUID()   = imageLayer->getUID();
                    newBinding.unit()        = unit.get();
                    newBinding.samplerName() = imageLayer->shareTexUniformName().get();
                    newBinding.matrixName()  = imageLayer->shareTexMatUniformName().get();

                    OE_INFO << LC 
                        << " .. Sampler=\"" << newBinding.samplerName() << "\", "
                        << "Matrix=\"" << newBinding.matrixName() << ", "
                        << "unit=" << newBinding.unit() << "\n";
                }
            }
        }

        else
        {
            // non-image tile layer. Keep track of these..
        }

        refresh();
    }
}


void
RexTerrainEngineNode::removeImageLayer( ImageLayer* layerRemoved )
{
    if ( layerRemoved )
    {
        // for a shared layer, release the shared image unit.
        if ( layerRemoved->getEnabled() && layerRemoved->isShared() )
        {
            if ( layerRemoved->shareImageUnit().isSet() )
            {
                getResources()->releaseTextureImageUnit( *layerRemoved->shareImageUnit() );
                layerRemoved->shareImageUnit().unset();
            }

            // Remove from RenderBindings (mark as unused)
            for (unsigned i = 0; i < _renderBindings.size(); ++i)
            {
                SamplerBinding& binding = _renderBindings[i];
                if (binding.isActive() && binding.sourceUID() == layerRemoved->getUID())
                {
                    binding.usage().clear();
                    binding.unit() = -1;
                }
            }
        }
    }

    refresh();
}

void
RexTerrainEngineNode::moveImageLayer( unsigned int oldIndex, unsigned int newIndex )
{
    updateState();
}

void
RexTerrainEngineNode::addElevationLayer( ElevationLayer* layer )
{
    if ( layer == 0L || layer->getEnabled() == false )
        return;

    layer->addCallback( _elevationCallback.get() );

    refresh();
}

void
RexTerrainEngineNode::removeElevationLayer( ElevationLayer* layerRemoved )
{
    if ( layerRemoved->getEnabled() == false )
        return;

    layerRemoved->removeCallback( _elevationCallback.get() );

    refresh();
}

void
RexTerrainEngineNode::moveElevationLayer( unsigned int oldIndex, unsigned int newIndex )
{
    refresh();
}

void
RexTerrainEngineNode::toggleElevationLayer( ElevationLayer* layer )
{
    refresh();
}

// Generates the main shader code for rendering the terrain.
void
RexTerrainEngineNode::updateState()
{
    if ( _batchUpdateInProgress )
    {
        _stateUpdateRequired = true;
    }
    else
    {
        osg::StateSet* terrainStateSet   = _terrain->getOrCreateStateSet();   // everything
        osg::StateSet* surfaceStateSet   = getSurfaceStateSet();    // just the surface
        
        //terrainStateSet->setRenderBinDetails(0, "SORT_FRONT_TO_BACK");
        
        // required for multipass tile rendering to work
        surfaceStateSet->setAttributeAndModes(
            new osg::Depth(osg::Depth::LEQUAL, 0, 1, true) );

        surfaceStateSet->setAttributeAndModes(
            new osg::CullFace(), osg::StateAttribute::ON);

        // activate standard mix blending.
        terrainStateSet->setAttributeAndModes( 
            new osg::BlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA),
            osg::StateAttribute::ON );

        // install patch param if we are tessellation on the GPU.
        if ( _terrainOptions.gpuTessellation() == true )
        {
            #ifdef HAVE_PATCH_PARAMETER
              terrainStateSet->setAttributeAndModes( new osg::PatchParameter(3) );
            #endif
        }

        // install shaders, if we're using them.
        if ( Registry::capabilities().supportsGLSL() )
        {
            Shaders package;

            VirtualProgram* terrainVP = VirtualProgram::getOrCreate(terrainStateSet);
            terrainVP->setName( "Rex Terrain" );
            package.load(terrainVP, package.ENGINE_VERT_MODEL);
            
            surfaceStateSet->addUniform(new osg::Uniform("oe_terrain_color", _terrainOptions.color().get()));

            bool useBlending = _terrainOptions.enableBlending().get();
            package.define("OE_REX_GL_BLENDING", useBlending);

            bool morphImagery = _terrainOptions.morphImagery().get();
            package.define("OE_REX_MORPH_IMAGERY", morphImagery);

            // Funtions that affect only the terrain surface:
            VirtualProgram* surfaceVP = VirtualProgram::getOrCreate(surfaceStateSet);
            surfaceVP->setName("Rex Surface");

            // Functions that affect the terrain surface only:
            package.load(surfaceVP, package.ENGINE_VERT_VIEW);
            package.load(surfaceVP, package.ENGINE_FRAG);

            // Normal mapping shaders:
            if ( this->normalTexturesRequired() )
            {
                package.load(surfaceVP, package.NORMAL_MAP_VERT);
                package.load(surfaceVP, package.NORMAL_MAP_FRAG);
            }

            // Morphing?
            if (_terrainOptions.morphTerrain() == true ||
                _terrainOptions.morphImagery() == true)
            {
                package.define("OE_REX_VERTEX_MORPHING", (_terrainOptions.morphTerrain() == true));
                package.load(surfaceVP, package.MORPHING_VERT);
            }

            // assemble color filter code snippets.
            bool haveColorFilters = false;
            {
                // Color filter frag function:
                std::string fs_colorfilters =
                    "#version " GLSL_VERSION_STR "\n"
                    GLSL_DEFAULT_PRECISION_FLOAT "\n"
                    "uniform int oe_layer_uid; \n"
                    "$COLOR_FILTER_HEAD"
                    "void oe_rexEngine_applyFilters(inout vec4 color) \n"
                    "{ \n"
                        "$COLOR_FILTER_BODY"
                    "} \n";

                std::stringstream cf_head;
                std::stringstream cf_body;
                const char* I = "    ";

                // second, install the per-layer color filter functions AND shared layer bindings.
                bool ifStarted = false;
                ImageLayerVector imageLayers;
                _update_mapf->getLayers(imageLayers);

                for( int i=0; i<imageLayers.size(); ++i )
                {
                    ImageLayer* layer = imageLayers.at(i);
                    if ( layer->getEnabled() )
                    {
                        // install Color Filter function calls:
                        const ColorFilterChain& chain = layer->getColorFilters();
                        if ( chain.size() > 0 )
                        {
                            haveColorFilters = true;
                            if ( ifStarted ) cf_body << I << "else if ";
                            else             cf_body << I << "if ";
                            cf_body << "(oe_layer_uid == " << layer->getUID() << ") {\n";
                            for( ColorFilterChain::const_iterator j = chain.begin(); j != chain.end(); ++j )
                            {
                                const ColorFilter* filter = j->get();
                                cf_head << "void " << filter->getEntryPointFunctionName() << "(inout vec4 color);\n";
                                cf_body << I << I << filter->getEntryPointFunctionName() << "(color);\n";
                                filter->install( surfaceStateSet );
                            }
                            cf_body << I << "}\n";
                            ifStarted = true;
                        }
                    }
                }

                if ( haveColorFilters )
                {
                    std::string cf_head_str, cf_body_str;
                    cf_head_str = cf_head.str();
                    cf_body_str = cf_body.str();

                    replaceIn( fs_colorfilters, "$COLOR_FILTER_HEAD", cf_head_str );
                    replaceIn( fs_colorfilters, "$COLOR_FILTER_BODY", cf_body_str );

                    surfaceVP->setFunction(
                        "oe_rexEngine_applyFilters",
                        fs_colorfilters,
                        ShaderComp::LOCATION_FRAGMENT_COLORING,
                        0.6 );
                }
            }

            // Apply uniforms for sampler bindings:
            OE_DEBUG << LC << "Render Bindings:\n";
            osg::ref_ptr<osg::Texture> tex = new osg::Texture2D(ImageUtils::createEmptyImage(1,1));
            for (unsigned i = 0; i < _renderBindings.size(); ++i)
            {
                SamplerBinding& b = _renderBindings[i];
                if (b.isActive())
                {
                    osg::Uniform* u = new osg::Uniform(b.samplerName().c_str(), b.unit());
                    terrainStateSet->addUniform( u );
                    OE_DEBUG << LC << " > Bound \"" << b.samplerName() << "\" to unit " << b.unit() << "\n";
                    terrainStateSet->setTextureAttribute(b.unit(), tex.get());
                }
            }

            //TODO: reevaluate these, since they may not persist when using direct
            // glUniform* calls in LayerDrawable etc.

            // uniform that controls per-layer opacity
            terrainStateSet->addUniform( new osg::Uniform("oe_layer_opacity", 1.0f) );

            // uniform that conveys the layer UID to the shaders; necessary
            // for per-layer branching (like color filters)
            // UID -1 => no image layer (no texture)
            terrainStateSet->addUniform( new osg::Uniform("oe_layer_uid", (int)-1 ) );

            // uniform that conveys the render order, since the shaders
            // need to know which is the first layer in order to blend properly
            terrainStateSet->addUniform( new osg::Uniform("oe_layer_order", (int)0) );

            // default min/max range uniforms. (max < min means ranges are disabled)
            terrainStateSet->addUniform( new osg::Uniform("oe_layer_minRange", 0.0f) );
            terrainStateSet->addUniform( new osg::Uniform("oe_layer_maxRange", -1.0f) );
            terrainStateSet->addUniform( new osg::Uniform("oe_layer_attenuationRange", _terrainOptions.attentuationDistance().get()) );
            
            terrainStateSet->getOrCreateUniform(
                "oe_min_tile_range_factor",
                osg::Uniform::FLOAT)->set( *_terrainOptions.minTileRangeFactor() );

            terrainStateSet->addUniform(new osg::Uniform("oe_tile_size", (float)_terrainOptions.tileSize().get()));

            // special object ID that denotes the terrain surface.
            surfaceStateSet->addUniform( new osg::Uniform(
                Registry::objectIndex()->getObjectIDUniformName().c_str(), OSGEARTH_OBJECTID_TERRAIN) );
        }

        _stateUpdateRequired = false;
    }
}
