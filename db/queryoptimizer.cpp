/* queryoptimizer.cpp */

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

#include "stdafx.h"
#include "pdfile.h"
#include "queryoptimizer.h"

namespace mongo {

    FieldBound::FieldBound( BSONElement e ) :
    lower_( minKey.firstElement() ),
    upper_( maxKey.firstElement() ) {
        if ( e.eoo() )
            return;
        if ( e.type() == RegEx ) {
            const char *r = e.simpleRegex();
            if ( r ) {
                lower_ = addObj( BSON( "" << r ) ).firstElement();
                upper_ = addObj( BSON( "" << simpleRegexEnd( r ) ) ).firstElement();
            }            
            return;
        }
        switch( e.getGtLtOp() ) {
            case JSMatcher::Equality:
                lower_ = e;
                upper_ = e;   
                break;
            case JSMatcher::LT:
            case JSMatcher::LTE:
                upper_ = e;
                break;
            case JSMatcher::GT:
            case JSMatcher::GTE:
                lower_ = e;
                break;
            case JSMatcher::opIN: {
                massert( "$in requires array", e.type() == Array );
                BSONElement max = minKey.firstElement();
                BSONElement min = maxKey.firstElement();
                BSONObjIterator i( e.embeddedObject() );
                while( i.more() ) {
                    BSONElement f = i.next();
                    if ( f.eoo() )
                        break;
                    if ( max.woCompare( f, false ) < 0 )
                        max = f;
                    if ( min.woCompare( f, false ) > 0 )
                        min = f;
                }
                lower_ = min;
                upper_ = max;
            }
            default:
                break;
        }
    }
    
    FieldBound &FieldBound::operator&=( const FieldBound &other ) {
        if ( other.upper_.woCompare( upper_, false ) < 0 )
            upper_ = other.upper_;
        if ( other.lower_.woCompare( lower_, false ) > 0 )
            lower_ = other.lower_;
        for( vector< BSONObj >::const_iterator i = other.objData_.begin(); i != other.objData_.end(); ++i )
            objData_.push_back( *i );
        massert( "Incompatible bounds", lower_.woCompare( upper_, false ) <= 0 );
        return *this;
    }
    
    string FieldBound::simpleRegexEnd( string regex ) {
        ++regex[ regex.length() - 1 ];
        return regex;
    }    
    
    BSONObj FieldBound::addObj( BSONObj o ) {
        objData_.push_back( o );
        return o;
    }
    
    FieldBoundSet::FieldBoundSet( BSONObj query ) :
    query_( query.copy() ) {
        BSONObjIterator i( query_ );
        while( i.more() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            if ( getGtLtOp( e ) == JSMatcher::Equality ) {
                bounds_[ e.fieldName() ] &= FieldBound( e );
            }
            else {
                BSONObjIterator i( e.embeddedObject() );
                while( i.more() ) {
                    BSONElement f = i.next();
                    if ( f.eoo() )
                        break;
                    bounds_[ e.fieldName() ] &= FieldBound( f );
                }                
            }
        }
    }
    
    FieldBound *FieldBoundSet::trivialBound_ = 0;
    FieldBound &FieldBoundSet::trivialBound() {
        if ( trivialBound_ == 0 )
            trivialBound_ = new FieldBound();
        return *trivialBound_;
    }
    
    QueryPlan::QueryPlan( const FieldBoundSet &fbs, BSONObj order, BSONObj idxKey ) :
    optimal_( false ),
    scanAndOrderRequired_( true ),
    keyMatch_( false ),
    exactKeyMatch_( false ) {
        // full table scan case
        if ( idxKey.isEmpty() ) {
            if ( order.isEmpty() )
                scanAndOrderRequired_ = false;
            return;
        }
        BSONObjIterator o( order );
        BSONObjIterator k( idxKey );
        int direction = 0;
        if ( !o.more() )
            scanAndOrderRequired_ = false;
        while( o.more() ) {
            BSONElement oe = o.next();
            if ( oe.eoo() ) {
                scanAndOrderRequired_ = false;
                break;
            }
            if ( !k.more() )
                break;
            BSONElement ke;
            while( 1 ) {
                ke = k.next();
                if ( ke.eoo() )
                    goto doneCheckOrder;
                if ( strcmp( oe.fieldName(), ke.fieldName() ) == 0 )
                    break;
                if ( !fbs.bound( ke.fieldName() ).equality() )
                    goto doneCheckOrder;
            }
            int d = oe.number() == ke.number() ? 1 : -1;
            if ( direction == 0 )
                direction = d;
            else if ( direction != d )
                break;          
        }
    doneCheckOrder:
        BSONObjIterator i( idxKey );
        int indexedQueryCount = 0;
        int exactIndexedQueryCount = 0;
        int orderEqIndexedQueryCount = 0;
        bool stillOrderEqIndexedQueryCount = true;
        set< string > orderFieldsUnindexed;
        order.getFieldNames( orderFieldsUnindexed );
        while( i.more() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            const FieldBound &fb = fbs.bound( e.fieldName() );
            if ( fb.nontrivial() )
                ++indexedQueryCount;
            if ( stillOrderEqIndexedQueryCount ) {
                if ( fb.equality() )
                    ++orderEqIndexedQueryCount;
                else
                    stillOrderEqIndexedQueryCount = false;
            }
            if ( fb.equality() ) {
                BSONElement e = fb.upper();
                if ( !e.isNumber() && !e.mayEncapsulate() && e.type() != RegEx )
                    ++exactIndexedQueryCount;
            }
            orderFieldsUnindexed.erase( e.fieldName() );
        }
        if ( !scanAndOrderRequired_ &&
            ( fbs.nNontrivialBounds() == 0 ||
             ( orderEqIndexedQueryCount > 0 &&
                orderEqIndexedQueryCount + 1 >= fbs.nNontrivialBounds() ) ) )
            optimal_ = true;
        if ( indexedQueryCount == fbs.nNontrivialBounds() &&
            orderFieldsUnindexed.size() == 0 ) {
            keyMatch_ = true;
            if ( exactIndexedQueryCount == fbs.nNontrivialBounds() )
                exactKeyMatch_ = true;
        }
    }
    
    QueryPlanSet::QueryPlanSet( const char *ns, BSONObj query, BSONObj order ) :
    fbs_( query ) {
        // Table scan plan
        plans_.push_back( QueryPlan( fbs_, order, emptyObj ) );

        if ( fbs_.nNontrivialBounds() == 0 && order.isEmpty() )
            return;
        
        NamespaceDetails *d = nsdetails( ns );
        assert( d );
        vector< QueryPlan > plans;
        for( int i = 0; i < d->nIndexes; ++i ) {
            QueryPlan p( fbs_, order, d->indexes[ i ].keyPattern() );
            if ( p.optimal() ) {
                plans_.push_back( p );
                return;
            }
            plans.push_back( p );
        }
        for( vector< QueryPlan >::iterator i = plans.begin(); i != plans.end(); ++i )
            plans_.push_back( *i );
    }
    
//    QueryPlan QueryOptimizer::getPlan(
//        const char *ns,
//        BSONObj* query,
//        BSONObj* order,
//        BSONObj* hint)
//    {
//        QueryPlan plan;
//
//
//
//        return plan;
//    }

} // namespace mongo
