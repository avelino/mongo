// d_migrate.cpp

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


/**
   these are commands that live in mongod
   mostly around shard management and checking
 */

#include "pch.h"
#include <map>
#include <string>

#include "../db/commands.h"
#include "../db/jsobj.h"
#include "../db/dbmessage.h"
#include "../db/query.h"

#include "../client/connpool.h"
#include "../client/distlock.h"

#include "../util/queue.h"

#include "shard.h"
#include "d_logic.h"
#include "config.h"
#include "chunk.h"

using namespace std;

namespace mongo {

    class ChunkCommandHelper : public Command {
    public:
        ChunkCommandHelper( const char * name ) 
            : Command( name ){
        }
        
        virtual void help( stringstream& help ) const {
            help << "internal should not be calling this directly" << endl;
        }
        virtual bool slaveOk() const { return false; }
        virtual bool adminOnly() const { return true; }
        virtual LockType locktype() const { return NONE; } 

    };


    class MigrateFromStatus {
    public:
        
        MigrateFromStatus()
            : _mutex( "MigrateFromStatus" ){
            _active = false;
            _inCriticalSection = false;
        }

        void start( string ns , const BSONObj& min , const BSONObj& max ){
            assert( ! _active );
            
            assert( ! min.isEmpty() );
            assert( ! max.isEmpty() );
            assert( ns.size() );
            
            _ns = ns;
            _min = min;
            _max = max;
            
            _deleted.clear();
            _reload.clear();
            
            _active = true;
        }
        
        void done(){
            if ( ! _active )
                return;
            _active = false;
            _inCriticalSection = false;

            scoped_lock lk( _mutex );
            _deleted.clear();
            _reload.clear();
        }
        
        void logOp( const char * opstr , const char * ns , const BSONObj& obj , BSONObj * patt ){
            if ( ! _active )
                return;

            if ( _ns != ns )
                return;
            
            char op = opstr[0];
            if ( op == 'n' || op =='c' || ( op == 'd' && opstr[1] == 'b' ) )
                return;

            BSONElement ide;
            if ( patt )
                ide = patt->getField( "_id" );
            else 
                ide = obj["_id"];
            
            if ( ide.eoo() ){
                log( LL_WARNING ) << "logOpForSharding got mod with no _id, ignoring  obj: " << obj << endl;
                return;
            }
            
            BSONObj it;

            switch ( opstr[0] ){
                
            case 'd': {
                // can't filter deletes :(
                scoped_lock lk( _mutex );
                _deleted.push_back( ide.wrap() );
                return;
            }
                
            case 'i': 
                it = obj;
                break;
                
            case 'u': 
                if ( ! Helpers::findById( cc() , _ns.c_str() , ide.wrap() , it ) ){
                    log( LL_WARNING ) << "logOpForSharding couldn't find: " << ide << " even though should have" << endl;
                    return;
                }
                break;
                
            }

            BSONObj k = it.extractFields( _min, true );

            if ( k.woCompare( _min ) < 0 ||
                 k.woCompare( _max ) >= 0 )
                return;
            
            scoped_lock lk( _mutex );
            _reload.push_back( ide.wrap() );
        }

        void xfer( list<BSONObj> * l , BSONObjBuilder& b , const char * name , long long& size , bool explode ){
            static long long maxSize = 1024 * 1024;
            
            if ( l->size() == 0 || size > maxSize )
                return;
            
            BSONArrayBuilder arr(b.subarrayStart(name));
            
            list<BSONObj>::iterator i = l->begin(); 
            
            while ( i != l->end() && size < maxSize ){
                BSONObj t = *i;
                if ( explode ){
                    BSONObj it;
                    if ( Helpers::findById( cc() , _ns.c_str() , t, it ) ){
                        arr.append( it );
                    }
                }
                else {
                    arr.append( t );
                }
                i = l->erase( i );
                size += t.objsize();
            }
            
            arr.done();
        }

        bool transferMods( string& errmsg , BSONObjBuilder& b ){
            if ( ! _active ){
                errmsg = "no active migration!";
                return false;
            }

            long long size = 0;

            {
                readlock rl( _ns );
                Client::Context cx( _ns );
                
                scoped_lock lk( _mutex );
                xfer( &_deleted , b , "deleted" , size , false );
                xfer( &_reload , b , "reload" , size , true );
            }

            b.append( "size" , size );

            return true;
        }

        bool _inCriticalSection;

    private:
        
        bool _active;

        string _ns;
        BSONObj _min;
        BSONObj _max;

        list<BSONObj> _reload;
        list<BSONObj> _deleted;

        mongo::mutex _mutex;
        
    } migrateFromStatus;
    
    struct MigrateStatusHolder {
        MigrateStatusHolder( string ns , const BSONObj& min , const BSONObj& max ){
            migrateFromStatus.start( ns , min , max );
        }
        ~MigrateStatusHolder(){
            migrateFromStatus.done();
        }
    };

    void logOpForSharding( const char * opstr , const char * ns , const BSONObj& obj , BSONObj * patt ){
        migrateFromStatus.logOp( opstr , ns , obj , patt );
    }

    class TransferModsCommand : public ChunkCommandHelper{
    public:
        TransferModsCommand() : ChunkCommandHelper( "_transferMods" ){}

        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
            return migrateFromStatus.transferMods( errmsg, result );
        }
    } transferModsCommand;

    /**
     * this is the main entry for moveChunk
     * called to initial a move
     * usually by a mongos
     * this is called on the "from" side
     */
    class MoveChunkCommand : public Command {
    public:
        MoveChunkCommand() : Command( "moveChunk" ){}
        virtual void help( stringstream& help ) const {
            help << "should not be calling this directly" << endl;
        }

        virtual bool slaveOk() const { return false; }
        virtual bool adminOnly() const { return true; }
        virtual LockType locktype() const { return NONE; } 
        
        
        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
            // 1. parse options
            // 2. make sure my view is complete and lock
            // 3. start migrate
            // 4. pause till migrate caught up
            // 5. LOCK
            //    a) update my config, essentially locking
            //    b) finish migrate
            //    c) update config server
            //    d) logChange to config server
            // 6. wait for all current cursors to expire
            // 7. remove data locally
            
            // -------------------------------
            
            // 1.
            string ns = cmdObj.firstElement().String();
            string to = cmdObj["to"].String();
            string from = cmdObj["from"].String(); // my public address, a tad redundant, but safe
            BSONObj min  = cmdObj["min"].Obj();
            BSONObj max  = cmdObj["max"].Obj();
            BSONElement shardId = cmdObj["shardId"];
            
            if ( ns.empty() ){
                errmsg = "need to specify namespace in command";
                return false;
            }
            
            if ( to.empty() ){
                errmsg = "need to specify server to move shard to";
                return false;
            }
            if ( from.empty() ){
                errmsg = "need to specify server to move shard from (redundat i know)";
                return false;
            }
            
            if ( min.isEmpty() ){
                errmsg = "need to specify a min";
                return false;
            }

            if ( max.isEmpty() ){
                errmsg = "need to specify a max";
                return false;
            }
            
            if ( shardId.eoo() ){
                errmsg = "need shardId";
                return false;
            }
            
            if ( ! shardingState.enabled() ){
                if ( cmdObj["configdb"].type() != String ){
                    errmsg = "sharding not enabled";
                    return false;
                }
                string configdb = cmdObj["configdb"].String();
                shardingState.enable( configdb );
                configServer.init( configdb );
            }



            Shard fromShard( from );
            Shard toShard( to );
            
            log() << "got movechunk: " << cmdObj << endl;
        
            // 2. 
            
            DistributedLock lockSetup( ConnectionString( shardingState.getConfigServer() , ConnectionString::SYNC ) , ns );
            dist_lock_try dlk( &lockSetup , (string)"migrate-" + min.toString() );
            if ( ! dlk.got() ){
                errmsg = "someone else has the lock";
                result.append( "who" , dlk.other() );
                return false;
            }

            ShardChunkVersion maxVersion;
            string myOldShard;
            {
                ScopedDbConnection conn( shardingState.getConfigServer() );
                
                BSONObj x = conn->findOne( ShardNS::chunk , Query( BSON( "ns" << ns ) ).sort( BSON( "lastmod" << -1 ) ) );
                maxVersion = x["lastmod"];

                x = conn->findOne( ShardNS::chunk , shardId.wrap( "_id" ) );
                myOldShard = x["shard"].String();
                
                if ( myOldShard != fromShard.getName() ){
                    errmsg = "i'm out of date";
                    result.append( "from" , fromShard.getName() );
                    result.append( "official" , myOldShard );
                    return false;
                }
                
                if ( maxVersion < shardingState.getVersion( ns ) ){
                    errmsg = "official version less than mine?";;
                    result.appendTimestamp( "officialVersion" , maxVersion );
                    result.appendTimestamp( "myVersion" , shardingState.getVersion( ns ) );
                    return false;
                }

                conn.done();
            }
            
            

            // 3.
            MigrateStatusHolder statusHolder( ns , min , max );
            
            {
                
                ScopedDbConnection conn( to );
                BSONObj res;
                bool ok = conn->runCommand( "admin" , 
                                            BSON( "_recvChunkStart" << ns <<
                                                  "from" << from <<
                                                  "min" << min <<
                                                  "max" << max
                                                  ) , 
                                            res );
                conn.done();

                if ( ! ok ){
                    errmsg = "_recvChunkStart failed: ";
                    errmsg += res["errmsg"].String();
                    result.append( "cause" , res );
                    return false;
                }

            }
            
            // 4. 
            for ( int i=0; i<86400; i++ ){ // don't want a single chunk move to take more than a day
                sleepsecs( 1 ); 
                ScopedDbConnection conn( to );
                BSONObj res;
                conn->runCommand( "admin" , BSON( "_recvChunkStatus" << 1 ) , res );
                res = res.getOwned();
                conn.done();
                
                log(0) << "_recvChunkStatus : " << res << endl;
                
                if ( res["state"].String() == "steady" )
                    break;
            }
            
            // 5.
            { 
                // 5.a
                migrateFromStatus._inCriticalSection = true;
                ShardChunkVersion myVersion = maxVersion;
                ++myVersion;
                
                {
                    dblock lk;
                    assert( myVersion > shardingState.getVersion( ns ) );
                    shardingState.setVersion( ns , myVersion );
                    assert( myVersion == shardingState.getVersion( ns ) );
                    log() << "moveChunk locking myself to: " << myVersion << endl;
                }

                
                // 5.b
                {
                    BSONObj res;
                    ScopedDbConnection conn( to );
                    bool ok = conn->runCommand( "admin" , 
                                                BSON( "_recvChunkCommit" << 1 ) ,
                                                res );
                    conn.done();

                    if ( ! ok ){
                        log() << "_recvChunkCommit failed: " << res << endl;
                        errmsg = "_recvChunkCommit failed!";
                        result.append( "cause" , res );
                        return false;
                    }
                }
                
                // 5.c
                ScopedDbConnection conn( shardingState.getConfigServer() );
                
                BSONObjBuilder temp;
                temp.append( "shard" , toShard.getName() );
                temp.appendTimestamp( "lastmod" , myVersion );

                conn->update( ShardNS::chunk , shardId.wrap( "_id" ) , BSON( "$set" << temp.obj() ) );
                
                { 
                    // update another random chunk
                    BSONObj x = conn->findOne( ShardNS::chunk , Query( BSON( "ns" << ns << "shard" << myOldShard ) ).sort( BSON( "lastmod" << -1 ) ) );
                    if ( ! x.isEmpty() ){
                        
                        BSONObjBuilder temp2;
                        ++myVersion;
                        temp2.appendTimestamp( "lastmod" , myVersion );
                        
                        shardingState.setVersion( ns , myVersion );

                        conn->update( ShardNS::chunk , x["_id"].wrap() , BSON( "$set" << temp2.obj() ) );
                        
                        log() << "moveChunk updating self to: " << myVersion << endl;
                    }
                    else {
                        //++myVersion;
                        shardingState.setVersion( ns , 0 );

                        log() << "moveChunk now i'm empty" << endl;
                    }
                }

                conn.done();
                migrateFromStatus._inCriticalSection = false;
                // 5.d
                configServer.logChange( "moveChunk" , ns , BSON( "min" << min << "max" << max <<
                                                                 "from" << fromShard.getName() << 
                                                                 "to" << toShard.getName() ) );
            }
            
            migrateFromStatus.done();
            // 6. 
            log( LL_WARNING ) << " deleting data before ensuring no more cursors TODO" << endl;
            
            // 7
            {
                writelock lk(ns);
                long long num = Helpers::removeRange( ns , min , max , true );
                log() << "moveChunk deleted: " << num << endl;
                result.appendNumber( "numDeleted" , num );
            }

            return true;
            
        }
        
    } moveChunkCmd;

    bool ShardingState::inCriticalMigrateSection(){
        return migrateFromStatus._inCriticalSection;
    }

    /* -----
       below this are the "to" side commands
       
       command to initiate
       worker thread
         does initial clone
         pulls initial change set
         keeps pulling
         keeps state
       command to get state
       commend to "commit"
    */

    class MigrateStatus {
    public:
        
        MigrateStatus(){
            active = false;
        }

        void prepare(){
            assert( ! active );
            state = READY;
            errmsg = "";

            numCloned = 0;
            numCatchup = 0;
            numSteady = 0;

            active = true;
        }

        void go(){
            try {
                _go();
            }
            catch ( std::exception& e ){
                state = FAIL;
                errmsg = e.what();
            }
            catch ( ... ){
                state = FAIL;
                errmsg = "UNKNOWN ERROR";
            }
            active = false;
        }

        void _go(){
            assert( active );
            assert( state == READY );
            assert( ! min.isEmpty() );
            assert( ! max.isEmpty() );

            ScopedDbConnection conn( from );
            conn->getLastError(); // just test connection
            
            state = CLONE;
            {
                auto_ptr<DBClientCursor> cursor = conn->query( ns , Query().minKey( min ).maxKey( max ) , /* QueryOption_Exhaust */ 0 );
                while ( cursor->more() ){
                    BSONObj o = cursor->next();
                    {
                        writelock lk( ns );
                        Helpers::upsert( ns , o );
                    }
                    numCloned++;
                }
            }
            
            {
                
                auto_ptr<DBClientCursor> indexes = conn->getIndexes( ns );
                vector<BSONObj> all;
                while ( indexes->more() ){
                    all.push_back( indexes->next().getOwned() );
                }
                
                writelock lk( ns );
                Client::Context ct( ns );
                
                string system_indexes = cc().database()->name + ".system.indexes";
                for ( unsigned i=0; i<all.size(); i++ ){
                    BSONObj idx = all[i];
                    theDataFileMgr.insert( system_indexes.c_str() , idx.objdata() , idx.objsize() );
                }
            }

            state = CATCHUP;
            while ( true ){
                BSONObj res;
                assert( conn->runCommand( "admin" , BSON( "_transferMods" << 1 ) , res ) );
                if ( res["size"].number() == 0 )
                    break;
                
                apply( res );
            }


            state = STEADY;
            while ( state == STEADY || state == COMMIT_START ){
                sleepmillis( 20 );

                BSONObj res;
                if ( ! conn->runCommand( "admin" , BSON( "_transferMods" << 1 ) , res ) ){
                    log() << "_transferMods failed in STEADY state: " << res << endl;
                    errmsg = res.toString();
                    state = FAIL;
                    return;
                }
                if ( res["size"].number() > 0 ){
                    apply( res );
                }

                if ( state == COMMIT_START )
                    break;
            }

            state = DONE;
            conn.done();
        }

        void status( BSONObjBuilder& b ){
            b.appendBool( "active" , active );
            if ( ! active )
                return;

            b.append( "ns" , ns );
            b.append( "from" , from );
            b.append( "min" , min );
            b.append( "max" , max );

            b.append( "state" , stateString() );

            {
                BSONObjBuilder bb( b.subobjStart( "counts" ) );
                bb.append( "cloned" , numCloned );
                bb.append( "catchup" , numCatchup );
                bb.append( "steady" , numSteady );
                bb.done();
            }


        }

        void apply( const BSONObj& xfer ){
            if ( xfer["deleted"].isABSONObj() ){
                writelock lk(ns);
                Client::Context cx(ns);
            
                // TODO:
                assert(0);
            }
            
            if ( xfer["reload"].isABSONObj() ){
                writelock lk(ns);
                Client::Context cx(ns);

                BSONObjIterator i( xfer["reload"].Obj() );
                while ( i.more() ){
                    BSONObj it = i.next().Obj();
                    Helpers::upsert( ns , it );
                }
            }
        }
        
        string stateString(){
            switch ( state ){
            case READY: return "ready";
            case CLONE: return "clone";
            case CATCHUP: return "catchup";
            case STEADY: return "steady";
            case COMMIT_START: return "commitStart";
            case DONE: return "done";
            case FAIL: return "fail";
            }
            assert(0);
            return "";
        }

        bool startCommit(){
            if ( state != STEADY )
                return false;
            state = COMMIT_START;
            
            for ( int i=0; i<86400; i++ ){
                sleepmillis(1);
                if ( state == DONE )
                    return true;
            }
            log() << "startCommit never finished!" << endl;
            return false;
        }

        bool active;
        
        string ns;
        string from;
        
        BSONObj min;
        BSONObj max;
        
        long long numCloned;
        long long numCatchup;
        long long numSteady;

        enum State { READY , CLONE , CATCHUP , STEADY , COMMIT_START , DONE , FAIL } state;
        string errmsg;
        
    } migrateStatus;
    
    void migrateThread(){
        Client::initThread( "migrateThread" );
        migrateStatus.go();
        cc().shutdown();
    }
    
    class RecvChunkStartCommand : public ChunkCommandHelper {
    public:
        RecvChunkStartCommand() : ChunkCommandHelper( "_recvChunkStart" ){}

        virtual LockType locktype() const { return WRITE; }  // this is so don't have to do locking internally

        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){

            if ( migrateStatus.active ){
                errmsg = "migrate already in progress";
                return false;
            }

            migrateStatus.prepare();

            migrateStatus.ns = cmdObj.firstElement().String();
            migrateStatus.from = cmdObj["from"].String();
            migrateStatus.min = cmdObj["min"].Obj().getOwned();
            migrateStatus.max = cmdObj["max"].Obj().getOwned();
            
            // TODO: check data in range currently

            boost::thread m( migrateThread );
            
            result.appendBool( "started" , true );
            return true;
        }

    } recvChunkStartCmd;

    class RecvChunkStatusCommand : public ChunkCommandHelper {
    public:
        RecvChunkStatusCommand() : ChunkCommandHelper( "_recvChunkStatus" ){}

        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
            migrateStatus.status( result );
            return 1;
        }
        
    } recvChunkStatusCommand;

    class RecvChunkCommitCommand : public ChunkCommandHelper {
    public:
        RecvChunkCommitCommand() : ChunkCommandHelper( "_recvChunkCommit" ){}
        
        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
            bool ok = migrateStatus.startCommit();
            migrateStatus.status( result );
            return ok;
        }

    } recvChunkCommitCommand;
    
    
}