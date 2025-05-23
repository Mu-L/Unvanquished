/*
===========================================================================

Unvanquished GPL Source Code
Copyright (C) 1999-2005 Id Software, Inc.
Copyright (C) 2000-2009 Darklegion Development

This file is part of the Unvanquished GPL Source Code (Unvanquished Source Code).

Unvanquished is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Unvanquished is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Unvanquished; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

===========================================================================
*/

// sg_weapon.c
// perform the server side effects of a weapon firing

#include "common/Common.h"
#include "sg_local.h"
#include "Entities.h"
#include "CBSE.h"

static void SendHitEvent( gentity_t *attacker, gentity_t *target, glm::vec3 const& origin, glm::vec3 const&  normal, entity_event_t evType );

static bool TakesDamages( gentity_t const* ent )
{
	return false
		|| Entities::IsAlive( ent )
		|| ( ent && ent->s.eType == entityType_t::ET_MOVER && ent->health > 0 )
		;
}

void G_ForceWeaponChange( gentity_t *ent, weapon_t weapon )
{
	playerState_t *ps = &ent->client->ps;

	if ( weapon == WP_NONE || !BG_InventoryContainsWeapon( weapon, ps->stats ) )
	{
		// switch to the first non blaster weapon
		ps->persistant[ PERS_NEWWEAPON ] = BG_PrimaryWeapon( ps->stats );
	}
	else
	{
		ps->persistant[ PERS_NEWWEAPON ] = weapon;
	}

	// force this here to prevent flamer effect from continuing
	ps->generic1 = WPM_NOTFIRING;

	// The PMove will do an animated drop, raise, and set the new weapon
	ps->pm_flags |= PMF_WEAPON_SWITCH;
}

/**
 * @brief Refills spare ammo clips.
 */
static void GiveMaxClips( gentity_t *self )
{
	playerState_t *ps;
	const weaponAttributes_t *wa;

	if ( !self || !self->client )
	{
		return;
	}

	self->client->lastAmmoRefillTime = level.time;

	ps = &self->client->ps;
	wa = BG_Weapon( ps->stats[ STAT_WEAPON ] );

	ps->clips = wa->maxClips;
}

/**
 * @brief Refills current ammo clip/charge.
 */
static void GiveFullClip( gentity_t *self )
{
	playerState_t *ps;
	const weaponAttributes_t *wa;

	if ( !self || !self->client )
	{
		return;
	}

	self->client->lastAmmoRefillTime = level.time;

	ps = &self->client->ps;
	wa = BG_Weapon( ps->stats[ STAT_WEAPON ] );

	ps->ammo = wa->maxAmmo;
}

/**
 * @brief Refills both spare clips and current clip/charge.
 */
void G_GiveMaxAmmo( gentity_t *self )
{
	GiveMaxClips( self );
	GiveFullClip( self );
}

/**
 * @brief Checks the condition for G_RefillAmmo.
 */
static bool CanUseAmmoRefill( gentity_t *self )
{
	const weaponAttributes_t *wa;
	playerState_t *ps;

	if ( !self || !self->client )
	{
		return false;
	}

	ps = &self->client->ps;
	wa = BG_Weapon( BG_PrimaryWeapon( ps->stats ) );

	if ( wa->infiniteAmmo )
	{
		return false;
	}

	return ps->clips != wa->maxClips || ps->ammo != wa->maxAmmo;
}

/**
 * @brief Refills clips on clip based weapons, refills charge on other weapons.
 * @param self
 * @param triggerEvent Trigger an event when relevant resource was modified.
 * @return Whether relevant resource was modified.
 */
bool G_RefillAmmo( gentity_t *self, bool triggerEvent )
{
	if ( !CanUseAmmoRefill( self ) )
	{
		return false;
	}

	if ( BG_Weapon( self->client->ps.stats[ STAT_WEAPON ] )->maxClips > 0 )
	{
		GiveMaxClips( self );
		GiveFullClip( self );

		if ( triggerEvent )
		{
			G_AddEvent( self, EV_AMMO_REFILL, 0 );
		}
	}
	else
	{
		GiveFullClip( self );

		if ( triggerEvent )
		{
			G_AddEvent( self, EV_AMMO_REFILL, 0 );
		}
	}

	G_ForceWeaponChange( self, (weapon_t) self->client->ps.weapon );

	return true;
}

/**
 * @brief Refills jetpack fuel.
 * @param self
 * @param triggerEvent Trigger an event when fuel was modified.
 * @return Whether fuel was modified.
 */
bool G_RefillFuel( gentity_t *self, bool triggerEvent )
{
	if ( !self || !self->client )
	{
		return false;
	}

	// needs a human with jetpack
	if ( G_Team( self) != TEAM_HUMANS ||
	     !BG_InventoryContainsUpgrade( UP_JETPACK, self->client->ps.stats ) )
	{
		return false;
	}

	if ( self->client->ps.stats[ STAT_FUEL ] != JETPACK_FUEL_MAX )
	{
		self->client->ps.stats[ STAT_FUEL ] = JETPACK_FUEL_MAX;

		self->client->lastFuelRefillTime = level.time;

		if ( triggerEvent )
		{
			G_AddEvent( self, EV_FUEL_REFILL, 0 );
		}

		return true;
	}
	else
	{
		return false;
	}
}

/**
 * @brief Attempts to refill ammo from a close source.
 * @return Whether ammo was refilled.
 */
bool G_FindAmmo( gentity_t *self )
{
	gentity_t *neighbor = nullptr;
	bool  foundSource = false;

	// don't search for a source if refilling isn't possible
	if ( !CanUseAmmoRefill( self ) )
	{
		return false;
	}

	// search for ammo source
	while ( ( neighbor = G_IterateEntitiesWithinRadius( neighbor, VEC2GLM( self->s.origin ), ENTITY_USE_RANGE ) ) )
	{
		// only friendly, living and powered buildables provide ammo
		if ( neighbor->s.eType != entityType_t::ET_BUILDABLE || !G_OnSameTeam( self, neighbor ) ||
		     !neighbor->spawned || !neighbor->powered || Entities::IsDead( neighbor ) )
		{
			continue;
		}

		switch ( neighbor->s.modelindex )
		{
			case BA_H_ARMOURY:
				foundSource = true;
				break;

			case BA_H_DRILL:
			case BA_H_REACTOR:
				if ( BG_Weapon( self->client->ps.stats[ STAT_WEAPON ] )->usesEnergy )
				{
					foundSource = true;
				}
				break;
		}
	}

	if ( foundSource )
	{
		return G_RefillAmmo( self, true );
	}

	return false;
}

/**
 * @brief Attempts to refill jetpack fuel from a close source.
 * @return true if fuel was refilled.
 */
bool G_FindFuel( gentity_t *self )
{
	gentity_t *neighbor = nullptr;
	bool  foundSource = false;

	if ( !self || !self->client )
	{
		return false;
	}

	// search for fuel source
	while ( ( neighbor = G_IterateEntitiesWithinRadius( neighbor, VEC2GLM( self->s.origin ), ENTITY_USE_RANGE ) ) )
	{
		// only friendly, living and powered buildables provide fuel
		if ( neighbor->s.eType != entityType_t::ET_BUILDABLE || !G_OnSameTeam( self, neighbor ) ||
		     !neighbor->spawned || !neighbor->powered || Entities::IsDead( neighbor ) )
		{
			continue;
		}

		switch ( neighbor->s.modelindex )
		{
			case BA_H_ARMOURY:
				foundSource = true;
				break;
		}
	}

	if ( foundSource )
	{
		return G_RefillFuel( self, true );
	}

	return false;
}

/*
================
Trace a bounding box against entities, but not the world
Also check there is a line of sight between the start and end point
FIXME: does not work correctly if width or height is big enough that the trace box
is not contained inside the attacking player's bounding box. The trace protrudes in the +z
direction for painsaw and all primary attacks of marauder and larger aliens. This means
if an enemy is standing on your head with these weapons the trace does not hit although
it should be in range.
================
*/
static void G_WideTrace(
		trace_t *tr, gentity_t *ent, const glm::vec3& muzzle, const glm::vec3& forward,
		const float range, const float width, const float height, gentity_t **target )
{
	float  halfDiagonal;

	*target = nullptr;

	if ( !ent->client )
	{
		return;
	}

	// Calculate box to use for trace
	glm::vec3 maxs{ width, width, height };
	glm::vec3 mins = -maxs;
	halfDiagonal = glm::length( maxs );

	G_UnlaggedOn( ent, GLM4READ( muzzle ), range + halfDiagonal );

	// Trace box against entities
	glm::vec3 end;
	VectorMA( muzzle, range, forward, end );
	trap_Trace( tr, muzzle, mins, maxs, end, ent->s.number, CONTENTS_BODY, 0 );

	if ( tr->entityNum != ENTITYNUM_NONE )
	{
		*target = &g_entities[ tr->entityNum ];
	}

	// Line trace against the world, so we never hit through obstacles.
	// The range is reduced according to the former trace so we don't hit something behind the
	// current target.
	glm::vec3 absDir = glm::max( glm::vec3( 1.0e-9f ), glm::abs( forward ) );
	glm::vec3 elementwiseDistToSide = maxs / absDir;
	// distToSide is the distance from the center of the trace box to the intersection of the trace line
	// and a side of the box. Should be between min(width, height) and halfDiagonal
	float distToSide = std::min({elementwiseDistToSide.x, elementwiseDistToSide.y, elementwiseDistToSide.z});
	float scale = tr->fraction * range + distToSide;
	VectorMA( muzzle, scale, forward, end );
	trap_Trace( tr, muzzle, {}, {}, end, ent->s.number, CONTENTS_SOLID, 0 );

	// In case we hit a different target, which can happen if two potential targets are close,
	// switch to it, so we will end up with the target we were looking at.
	if ( tr->entityNum != ENTITYNUM_NONE )
	{
		*target = &g_entities[ tr->entityNum ];
	}

	G_UnlaggedOff();
}

/*
======================
Round a vector to integers for more efficient network
transmission, but make sure that it rounds towards a given point
rather than blindly truncating.  This prevents it from truncating
into a wall.
======================
*/
void G_SnapVectorTowards( vec3_t v, const vec3_t to )
{
	int i;

	for ( i = 0; i < 3; i++ )
	{
		if ( v[ i ] >= 0 )
		{
			v[ i ] = ( int )( v[ i ] + ( to[ i ] <= v[ i ] ? 0 : 1 ) );
		}
		else
		{
			v[ i ] = ( int )( v[ i ] + ( to[ i ] <= v[ i ] ? -1 : 0 ) );
		}
	}
}

void G_SnapVectorTowards(glm::vec3 &v, const glm::vec3 &to)
{
	int i;

	for (i = 0; i < 3; i++)
	{
		if (v[i] >= 0.0f)
		{
			v[i] = (int)(v[i] + (to[i] <= v[i] ? 0.0f : 1.0f));
		}
		else
		{
			v[i] = (int)(v[i] + (to[i] <= v[i] ? -1.0f : 0.0f));
		}
	}
}

static void SendRangedHitEvent( gentity_t *attacker, const glm::vec3 &muzzle, gentity_t *target, trace_t *tr )
{
	// snap the endpos to integers, but nudged towards the line
	glm::vec3 endpos = VEC2GLM(tr->endpos);
	G_SnapVectorTowards(endpos, muzzle);
	VectorCopy(endpos, tr->endpos);

	entity_event_t evType = HasComponents<HealthComponent>(*target->entity) ? EV_WEAPON_HIT_ENTITY : EV_WEAPON_HIT_ENVIRONMENT;
	SendHitEvent( attacker, target, endpos, VEC2GLM( tr->plane.normal ), evType );
}

static void SendHitEvent( gentity_t *attacker, gentity_t *target, glm::vec3 const& origin, glm::vec3 const&  normal, entity_event_t evType )
{
	gentity_t *event = G_NewTempEntity( origin, evType );

	// normal
	event->s.eventParm = DirToByte( GLM4READ( normal ) );

	// victim
	event->s.otherEntityNum = target->s.number;

	// attacker
	event->s.otherEntityNum2 = attacker->s.number;

	// weapon
	event->s.weapon = attacker->s.weapon;

	// weapon mode
	event->s.generic1 = attacker->s.generic1;
}

static void SendMeleeHitEvent( gentity_t *attacker, gentity_t *target, trace_t *tr )
{
	if ( !attacker->client )
	{
		return;
	}

	//tyrant charge attack do not have traces... there must be a better way for that...
	glm::vec3 normal = ( tr ? VEC2GLM( tr->endpos ) : VEC2GLM( attacker->client->ps.origin ) ) - VEC2GLM( target->s.origin );

	// Normalize the horizontal components of the vector difference to the "radius" of the bounding box
	float mag = sqrtf( normal[ 0 ] * normal[ 0 ] + normal[ 1 ] * normal[ 1 ] );
	float radius = target->r.maxs[ 0 ] * 1.21f;
	if ( mag > radius )
	{
		normal[ 0 ] = normal[ 0 ] / mag * radius;
		normal[ 1 ] = normal[ 1 ] / mag * radius;
	}
	normal[ 2 ] = Math::Clamp( normal[ 2 ], target->r.mins[ 2 ], target->r.maxs[ 2 ] );

	glm::vec3 origin = VEC2GLM( target->s.origin ) + normal;
	normal = -normal;
	VectorNormalize( normal );

	SendHitEvent( attacker, target, origin, normal, EV_WEAPON_HIT_ENTITY );
}

static gentity_t *FireMelee( gentity_t *self, float range, float width, float height,
                             float damage, meansOfDeath_t mod, bool falseRanged )
{
	trace_t   tr;
	gentity_t *traceEnt;

	glm::vec3 forward;
	AngleVectors( VEC2GLM( self->client->ps.viewangles ), &forward, nullptr, nullptr);
	glm::vec3 muzzle = G_CalcMuzzlePoint( self, forward );

	G_WideTrace( &tr, self, muzzle, forward, range, width, height, &traceEnt );

	if ( traceEnt == nullptr )
	{
		return nullptr;
	}

	bool sendDamage = TakesDamages( traceEnt );

	if ( sendDamage )
	{
		traceEnt->Damage( damage, self, VEC2GLM( tr.endpos ), forward, 0, mod );
	}

	// for painsaw. This makes really little sense to me, but this is refactoring, not bugsquashing.
	if ( falseRanged )
	{
		SendRangedHitEvent( self, muzzle, traceEnt, &tr );
	}
	else if ( sendDamage )
	{
		SendMeleeHitEvent( self, traceEnt, &tr );
	}

	return traceEnt;
}

/*
======================================================================

MACHINEGUN

======================================================================
*/

static void FireBullet( gentity_t *self, float spread, float damage, meansOfDeath_t mod, int other )
{
	trace_t   tr;
	glm::vec3 end;
	gentity_t *target;

	glm::vec3 forward, right, up, muzzle;
	if ( mod == MOD_MGTURRET )
	{
		AngleVectors( self->entity->Get<TurretComponent>()->GetAimAngles(), &forward, &right, &up );
		VectorCopy( self->s.pos.trBase, muzzle );
	}
	else
	{
		AngleVectors( VEC2GLM( self->client->ps.viewangles ), &forward, &right, &up );
		muzzle = G_CalcMuzzlePoint( self, forward );
	}

	VectorMA( muzzle, 8192 * 16, forward, end );
	if ( spread > 0.f )
	{
		float r = random() * M_PI * 2.0f;
		float u = sinf( r ) * crandom() * spread * 16;
		r = cosf( r ) * crandom() * spread * 16;
		VectorMA( end, r, right, end );
		VectorMA( end, u, up, end );
	}

	// don't use unlagged if this is not a client (e.g. turret)
	if ( self->client )
	{
		G_UnlaggedOn( self, GLM4READ( muzzle ), 8192 * 16 );
		trap_Trace( &tr, muzzle, {}, {}, end, self->s.number, MASK_SHOT, 0 );
		G_UnlaggedOff();
	}
	else
	{
		trap_Trace( &tr, muzzle, {}, {}, end, self->s.number, MASK_SHOT, 0 );
	}

	if ( tr.surfaceFlags & SURF_NOIMPACT )
	{
		return;
	}

	target = &g_entities[ tr.entityNum ];

	SendRangedHitEvent( self, muzzle, target, &tr );

	target->Damage(damage, self, VEC2GLM( tr.endpos ), forward, other, mod);
}

// spawns a missile at parent's muzzle going in forward dir
// missile: missile type
static void FireMissile( gentity_t* self, missile_t missile )
{
	glm::vec3 forward;
	AngleVectors( VEC2GLM( self->client->ps.viewangles ), &forward, nullptr, nullptr);
	glm::vec3 muzzle = G_CalcMuzzlePoint( self, forward );
	G_SpawnDumbMissile( missile, self, muzzle, forward );
}

/*
======================================================================

SHOTGUN

======================================================================
*/

/*
================
Keep this in sync with ShotgunPattern in CGAME!
================
*/
static void ShotgunPattern( glm::vec3 const& origin, glm::vec3 const& origin2, int seed, gentity_t *self )
{
	// derive the right and up vectors from the forward vector, because
	// the client won't have any other information
	glm::vec3 forward = glm::normalize( origin2 );
	//TODO: check if glm::perp() is equivalent.
	glm::vec3 right = PerpendicularVector( forward );
	// FIXME: the cross product of forward and right is DOWN not up!
	glm::vec3 up = glm::cross( forward, right );

	// generate the "random" spread pattern
	for ( int i = 0; i < SHOTGUN_PELLETS; i++ )
	{
		float r = Q_crandom( &seed ) * M_PI;
		float a = Q_random( &seed ) * SHOTGUN_SPREAD * 16;

		float u = sinf( r ) * a;
		r = cosf( r ) * a;

		glm::vec3 end = origin + float(SHOTGUN_RANGE) * forward;
		end += r * right;
		end += u * up;

		trace_t tr;
		trap_Trace( &tr, origin, glm::vec3(), glm::vec3(), end, self->s.number, MASK_SHOT, 0 );
		g_entities[ tr.entityNum ].Damage( (float)SHOTGUN_DMG, self, VEC2GLM( tr.endpos ),
		                                   forward, 0, MOD_SHOTGUN );
	}
}

static void FireShotgun( gentity_t *self ) //TODO merge with FireBullet
{
	gentity_t *tent;

	glm::vec3 forward;
	AngleVectors( VEC2GLM( self->client->ps.viewangles ), &forward, nullptr, nullptr);
	glm::vec3 muzzle = G_CalcMuzzlePoint( self, forward );

	// instead of an EV_WEAPON_HIT_* event, send this so client can generate the same spread pattern
	tent = G_NewTempEntity( muzzle, EV_SHOTGUN );
	VectorScale( forward, 4096, tent->s.origin2 );
	SnapVector( tent->s.origin2 );
	tent->s.eventParm = rand() / ( RAND_MAX / 0x100 + 1 ); // seed for spread pattern
	tent->s.otherEntityNum = self->s.number;

	// calculate the pattern and do the damage
	G_UnlaggedOn( self, GLM4READ( muzzle ), SHOTGUN_RANGE );
	ShotgunPattern( VEC2GLM( tent->s.pos.trBase ), VEC2GLM( tent->s.origin2 ), tent->s.eventParm, self );
	G_UnlaggedOff();
}

/*
======================================================================

FIREBOMB

======================================================================
*/

#define FIREBOMB_SUBMISSILE_COUNT 15
#define FIREBOMB_IGNITE_RANGE     192

void G_FirebombMissileIgnite( gentity_t *self )
{
	// ignite alien buildables in range
	gentity_t *neighbor = nullptr;
	while ( ( neighbor = G_IterateEntitiesWithinRadius( neighbor, VEC2GLM( self->s.origin ), FIREBOMB_IGNITE_RANGE ) ) )
	{
		if ( neighbor->s.eType == entityType_t::ET_BUILDABLE && G_Team( neighbor ) == TEAM_ALIENS &&
		     G_LineOfSight( self, neighbor ) )
		{
			neighbor->entity->Ignite( self->parent );
		}
	}

	// set floor below on fire (assumes the firebomb lays on the floor!)
	G_SpawnFire( self->s.origin, GLM4READ( glm::vec3( 0.f, 0.f, 1.f ) ), self->parent );

	// spam fire
	for ( int subMissilenum = 0; subMissilenum < FIREBOMB_SUBMISSILE_COUNT; subMissilenum++ )
	{
		glm::vec3 dir =
		{
			( rand() / static_cast<float>( RAND_MAX ) ) - 0.5f,
			( rand() / static_cast<float>( RAND_MAX ) ) - 0.5f,
			( rand() / static_cast<float>( RAND_MAX ) ) * 0.5f,
		};

		VectorNormalize( dir );

		// the submissile's parent is the attacker
		gentity_t *m = G_SpawnDumbMissile( MIS_FIREBOMB_SUB, self->parent, VEC2GLM( self->s.origin ), dir );

		// randomize missile speed
		VectorScale( m->s.pos.trDelta, ( rand() / ( float )RAND_MAX ) + 0.5f, m->s.pos.trDelta );
	}
}

/*
======================================================================

LUCIFER CANNON

======================================================================
*/

static void FireLcannonPrimary( gentity_t *self, int damage )
{
	// TODO: Tidy up this and lcannonFire

	gentity_t *m;
	float     charge;

	glm::vec3 dir;
	AngleVectors( VEC2GLM( self->client->ps.viewangles ), &dir, nullptr, nullptr );
	glm::vec3 start = G_CalcMuzzlePoint( self, dir );

	{
		missileAttributes_t attr = *BG_Missile( MIS_LCANNON );
		// some values are set in the code
		attr.damage = damage;
		attr.splashDamage = damage / 2;
		if ( damage == LCANNON_DAMAGE )
		{
			// Explode immediately when overcharged.
			// But beware glitchy and framerate-dependent behavior: despite exploding
			// "instantly" (in the next frame), it "travels" for a distance
			// (MISSILE_PRESTEP_TIME + length of 1 frame) � attr.speed
			// and can score direct hits against other entities
			attr.lifetime = 0;
		}

		m = G_NewEntity( HAS_CBSE );
		DumbMissileEntity::Params params;
		params.oldEnt = m;
		params.Missile_attributes = &attr;
		m->entity = new DumbMissileEntity{ params };
		G_SetUpMissile( m, self, GLM4READ( start ), GLM4READ( dir ) );

		// pass the missile charge through
		charge = ( float )( damage - BG_Missile( MIS_LCANNON2 )->damage ) / LCANNON_DAMAGE;

		m->s.torsoAnim = charge * 255;

		if ( m->s.torsoAnim < 0 )
		{
			m->s.torsoAnim = 0;
		}
	}
}

static void FireLcannon( gentity_t *self, bool secondary )
{
	if ( secondary && self->client->ps.weaponCharge <= 0 )
	{
		FireMissile( self, MIS_LCANNON2 );
	}
	else
	{
		FireLcannonPrimary( self, self->client->ps.weaponCharge * LCANNON_DAMAGE / LCANNON_CHARGE_TIME_MAX );
	}

	self->client->ps.weaponCharge = 0;
}

/*
======================================================================

BUILD GUN

======================================================================
*/

void G_CheckCkitRepair( gentity_t *self )
{
	if ( self->client->ps.weaponTime > 0 ||
	     self->client->ps.stats[ STAT_MISC ] > 0 )
	{
		return;
	}

	glm::vec3 viewOrigin = BG_GetClientViewOrigin( &self->client->ps );
	glm::vec3 forward;
	AngleVectors( VEC2GLM( self->client->ps.viewangles ), &forward, nullptr, nullptr );
	glm::vec3 end = viewOrigin + 100.f * forward;

	trace_t tr;
	trap_Trace( &tr, viewOrigin, glm::vec3(), glm::vec3(), end, self->s.number, MASK_PLAYERSOLID, 0 );
	gentity_t *traceEnt = &g_entities[ tr.entityNum ];

	if ( tr.fraction < 1.0f && traceEnt->spawned && traceEnt->s.eType == entityType_t::ET_BUILDABLE &&
	     G_Team( traceEnt ) == TEAM_HUMANS )
	{
		HealthComponent *healthComponent = traceEnt->entity->Get<HealthComponent>();

		if (healthComponent && healthComponent->Alive() && !healthComponent->FullHealth()) {
			traceEnt->entity->Heal(HBUILD_HEALRATE, nullptr);

			if (healthComponent->FullHealth()) {
				G_AddEvent(self, EV_BUILD_REPAIRED, 0);
			} else {
				G_AddEvent(self, EV_BUILD_REPAIR, 0);
			}

			self->client->ps.weaponTime += BG_Weapon( self->client->ps.weapon )->repeatRate1;
		}
	}
}

static void CancelBuild( gentity_t *self )
{
	// Cancel ghost buildable
	if ( self->client->ps.stats[ STAT_BUILDABLE ] != BA_NONE )
	{
		self->client->ps.stats[ STAT_BUILDABLE ] = BA_NONE;
		self->client->ps.stats[ STAT_PREDICTION ] = 0;
		return;
	}

	if ( self->client->ps.weapon == WP_ABUILD ||
	     self->client->ps.weapon == WP_ABUILD2 )
	{
		FireMelee( self, ABUILDER_CLAW_RANGE, ABUILDER_CLAW_WIDTH,
		           ABUILDER_CLAW_WIDTH, ABUILDER_CLAW_DMG, MOD_ABUILDER_CLAW, false );
	}
}

// do the same thing as /deconstruct
static void FireMarkDeconstruct( gentity_t *self )
{
	gentity_t* buildable = G_GetDeconstructibleBuildable( self );
	if ( buildable == nullptr )
	{
		return;
	}
	if ( G_DeconstructDead( buildable ) )
	{
		return;
	}
	buildable->entity->Get<BuildableComponent>()->ToggleDeconstructionMark();
}

static void DeconstructSelectTarget( gentity_t *self )
{
	gentity_t* target = G_GetDeconstructibleBuildable( self );
	if ( target == nullptr // No target found
		|| G_DeconstructDead( target ) // Successfully deconned dead target
		|| G_CheckDeconProtectionAndWarn( target, self ) ) // Not allowed to decon target
	{
		self->target = nullptr;
		// Stop the force-deconstruction charge bar
		self->client->pmext.cancelDeconstructCharge = true;
	}
	else
	{
		// Set the target which will be used upon reaching full charge
		self->target = target;
	}
}

static void FireForceDeconstruct( gentity_t *self )
{
	if ( !self->target )
	{
		return;
	}
	gentity_t* target = self->target.get();
	glm::vec3 viewOrigin = BG_GetClientViewOrigin( &self->client->ps );
	// The builder must still be in a range such that G_GetDeconstructibleBuildable could return
	// the buildable (but with 10% extra distance allowed).
	// However it is not necessary to be aiming at the target.
	if ( G_DistanceToBBox( viewOrigin, target ) > BUILDER_DECONSTRUCT_RANGE * 1.1f )
	{
		// Target is too far away.
		// TODO: Continuously check that target is valid (in range and still exists), rather
		// than only at the beginning and end of the charge.
		return;
	}

	if ( G_DeconstructDead( self->target.entity ) )
	{
		return;
	}
	G_DeconstructUnprotected( self->target.entity, self );
}

static void FireBuild( gentity_t *self, dynMenu_t menu )
{
	buildable_t buildable;

	if ( !self->client )
	{
		return;
	}

	buildable = (buildable_t) ( self->client->ps.stats[ STAT_BUILDABLE ] & SB_BUILDABLE_MASK );

	// open build menu
	if ( buildable <= BA_NONE )
	{
		G_TriggerMenu( self->num(), menu );
		return;
	}

	// can't build just yet
	if ( self->client->ps.stats[ STAT_MISC ] > 0 )
	{
		G_AddEvent( self, EV_BUILD_DELAY, self->num() );
		return;
	}

	// build
	if ( G_BuildIfValid( self, buildable ) )
	{
		if ( !g_instantBuilding.Get() )
		{
			int buildTime = BG_Buildable( buildable )->buildTime;

			switch ( G_Team( self ) )
			{
				case TEAM_ALIENS:
					buildTime *= ALIEN_BUILDDELAY_MOD;
					break;

				case TEAM_HUMANS:
					buildTime *= HUMAN_BUILDDELAY_MOD;
					break;

				default:
					break;
			}

			self->client->ps.stats[ STAT_MISC ] += buildTime;
		}

		self->client->ps.stats[ STAT_BUILDABLE ] = BA_NONE;
	}
}

/*
======================================================================

LEVEL0

======================================================================
*/

bool G_CheckDretchAttack( gentity_t *self )
{
	trace_t   tr;
	gentity_t *traceEnt;

	if ( self->client->ps.weaponTime )
	{
		return false;
	}

	// Calculate muzzle point
	glm::vec3 forward;
	AngleVectors( VEC2GLM( self->client->ps.viewangles ), &forward, nullptr, nullptr);
	glm::vec3 muzzle = G_CalcMuzzlePoint( self, forward );

	G_WideTrace( &tr, self, muzzle, forward, LEVEL0_BITE_RANGE, LEVEL0_BITE_WIDTH, LEVEL0_BITE_WIDTH, &traceEnt );

	//this is ugly, but so is all that mess in any case. I'm just trying to fix shit here, so go complain to whoever broke the game, not to me.

	if ( not TakesDamages( traceEnt )
				|| G_OnSameTeam( self, traceEnt )
				|| !G_DretchCanDamageEntity( traceEnt ) )
	{
		return false;
	}

	traceEnt->Damage((float)LEVEL0_BITE_DMG, self, VEC2GLM( tr.endpos ),
	                         forward, 0, (meansOfDeath_t)MOD_LEVEL0_BITE);

	SendMeleeHitEvent( self, traceEnt, &tr );

	self->client->ps.weaponTime += LEVEL0_BITE_REPEAT;

	return true;
}

/*
======================================================================

LEVEL2

======================================================================
*/
#define MAX_ZAPS MAX_CLIENTS

static zap_t zaps[ MAX_ZAPS ];

static void FindZapChainTargets( zap_t *zap )
{
	gentity_t *ent = zap->targets[ 0 ]; // the source
	int       entityList[ MAX_GENTITIES ];

	glm::vec3 range = { LEVEL2_AREAZAP_CHAIN_RANGE, LEVEL2_AREAZAP_CHAIN_RANGE, LEVEL2_AREAZAP_CHAIN_RANGE };
	glm::vec3 origin = VEC2GLM( ent->s.origin );
	glm::vec3 maxs = origin + range;
	glm::vec3 mins = origin - range;

	int num = trap_EntitiesInBox( GLM4READ( mins ), GLM4READ( maxs ), entityList, MAX_GENTITIES );

	for ( int i = 0; i < num; i++ )
	{
		gentity_t *enemy = &g_entities[ entityList[ i ] ];

		// don't chain to self; noclippers can be listed, don't chain to them either
		if ( enemy == ent || ( enemy->client && enemy->client->noclip ) )
		{
			continue;
		}

		float distance = glm::distance( origin, VEC2GLM( enemy->s.origin ) );

		//TODO: implement support for map-entities
		if ( G_Team( enemy ) == TEAM_HUMANS
				&& ( enemy->client || enemy->s.eType == entityType_t::ET_BUILDABLE )
				&& Entities::IsAlive( enemy )
				&& distance <= LEVEL2_AREAZAP_CHAIN_RANGE )
		{
			// world-LOS check: trace against the world, ignoring other BODY entities
			trace_t tr;
			trap_Trace( &tr, origin, glm::vec3(), glm::vec3(), VEC2GLM( enemy->s.origin ),
			            ent->s.number, CONTENTS_SOLID, 0 );

			if ( tr.entityNum == ENTITYNUM_NONE )
			{
				zap->targets[ zap->numTargets ] = enemy;
				zap->distances[ zap->numTargets ] = distance;

				if ( ++zap->numTargets >= LEVEL2_AREAZAP_MAX_TARGETS )
				{
					return;
				}
			}
		}
	}
}

// origin is just used for PVS determinations
static void UpdateZapEffect( zap_t *zap, const glm::vec3 &origin )
{
	int i;
	int entityNums[ LEVEL2_AREAZAP_MAX_TARGETS + 1 ];

	entityNums[ 0 ] = zap->creator->s.number;

	ASSERT_LE(zap->numTargets, LEVEL2_AREAZAP_MAX_TARGETS);

	for ( i = 0; i < zap->numTargets; i++ )
	{
		entityNums[ i + 1 ] = zap->targets[ i ]->s.number;
	}

	BG_PackEntityNumbers( &zap->effectChannel->s,
	                      entityNums, zap->numTargets + 1 );

	G_SetOrigin( zap->effectChannel, origin );
	trap_LinkEntity( zap->effectChannel );
}

static void CreateNewZap( gentity_t *creator, const glm::vec3 &muzzle,
                          const glm::vec3 &forward, gentity_t *target )
{
	int   i;
	zap_t *zap;

	for ( i = 0; i < MAX_ZAPS; i++ )
	{
		zap = &zaps[ i ];

		if ( zap->used )
		{
			continue;
		}

		zap->used = true;
		zap->timeToLive = LEVEL2_AREAZAP_TIME;

		zap->creator = creator;
		zap->targets[ 0 ] = target;
		zap->numTargets = 1;

		// Zap chains only originate from alive entities.
		if (target->Damage((float)LEVEL2_AREAZAP_DMG, creator, VEC2GLM( target->s.origin ),
		                           forward, DAMAGE_NO_LOCDAMAGE, MOD_LEVEL2_ZAP)) {
			FindZapChainTargets( zap );

			for ( i = 1; i < zap->numTargets; i++ )
			{
				float damage = LEVEL2_AREAZAP_DMG * ( 1 - powf( ( zap->distances[ i ] /
				               LEVEL2_AREAZAP_CHAIN_RANGE ), LEVEL2_AREAZAP_CHAIN_FALLOFF ) ) + 1;

				zap->targets[i]->Damage(damage, zap->creator, VEC2GLM( zap->targets[i]->s.origin ),
				                       forward, DAMAGE_NO_LOCDAMAGE, MOD_LEVEL2_ZAP);
			}
		}

		zap->effectChannel = G_NewEntity( NO_CBSE );
		zap->effectChannel->s.eType = entityType_t::ET_LEV2_ZAP_CHAIN;
		zap->effectChannel->classname = "lev2zapchain";
		UpdateZapEffect( zap, muzzle );

		return;
	}
}

void G_UpdateZaps( int msec )
{
	int   i, j;
	zap_t *zap;

	for ( i = 0; i < MAX_ZAPS; i++ )
	{
		zap = &zaps[ i ];

		if ( !zap->used )
		{
			continue;
		}

		zap->timeToLive -= msec;

		// first, the disappearance of players is handled immediately in G_ClearPlayerZapEffects()

		// the deconstruction or gibbing of a directly targeted buildable destroys the whole zap effect
		if ( zap->timeToLive <= 0 || !zap->targets[ 0 ]->inuse )
		{
			G_FreeEntity( zap->effectChannel );
			zap->used = false;
			continue;
		}

		// the deconstruction or gibbing of chained buildables destroy the appropriate beams
		for ( j = 1; j < zap->numTargets; j++ )
		{
			if ( !zap->targets[ j ]->inuse )
			{
				zap->targets[ j-- ] = zap->targets[ --zap->numTargets ];
			}
		}

		glm::vec3 attackerForward;
		AngleVectors( VEC2GLM( zap->creator->client->ps.viewangles ), &attackerForward, nullptr, nullptr );
		glm::vec3 attackerMuzzle = G_CalcMuzzlePoint( zap->creator, attackerForward );

		UpdateZapEffect( zap, attackerMuzzle );
	}
}

/*
===============
Called from G_LeaveTeam() and TeleportPlayer().
===============
*/
void G_ClearPlayerZapEffects( gentity_t *player )
{
	int   i, j;
	zap_t *zap;

	for ( i = 0; i < MAX_ZAPS; i++ )
	{
		zap = &zaps[ i ];

		if ( !zap->used )
		{
			continue;
		}

		// the disappearance of the creator or the first target destroys the whole zap effect
		if ( zap->creator == player || zap->targets[ 0 ] == player )
		{
			G_FreeEntity( zap->effectChannel );
			zap->used = false;
			continue;
		}

		// the disappearance of chained players destroy the appropriate beams
		for ( j = 1; j < zap->numTargets; j++ )
		{
			if ( zap->targets[ j ] == player )
			{
				zap->targets[ j-- ] = zap->targets[ --zap->numTargets ];
			}
		}
	}
}

static void FireAreaZap( gentity_t *ent )
{
	trace_t   tr;
	gentity_t *traceEnt;

	glm::vec3 forward;
	AngleVectors( VEC2GLM( ent->client->ps.viewangles ), &forward, nullptr, nullptr);
	glm::vec3 muzzle = G_CalcMuzzlePoint( ent, forward );

	G_WideTrace( &tr, ent, muzzle, forward, LEVEL2_AREAZAP_RANGE, LEVEL2_AREAZAP_WIDTH, LEVEL2_AREAZAP_WIDTH, &traceEnt );

	if ( G_Team( traceEnt ) == TEAM_HUMANS &&
			( traceEnt->client || traceEnt->s.eType == entityType_t::ET_BUILDABLE ) )
	{
		CreateNewZap( ent, muzzle, forward, traceEnt );
	}
}

/*
======================================================================

LEVEL3

======================================================================
*/

bool G_CheckPounceAttack( gentity_t *self )
{
	trace_t   tr;
	gentity_t *traceEnt;
	int       damage, timeMax, pounceRange, payload;

	if ( self->client->pmext.pouncePayload <= 0 )
	{
		return false;
	}

	// In case the goon lands on his target, he gets one shot after landing
	payload = self->client->pmext.pouncePayload;

	if ( !( self->client->ps.pm_flags & PMF_CHARGE ) )
	{
		self->client->pmext.pouncePayload = 0;
	}

	// Calculate muzzle point
	glm::vec3 forward;
	AngleVectors( VEC2GLM( self->client->ps.viewangles ), &forward, nullptr, nullptr);
	glm::vec3 muzzle = G_CalcMuzzlePoint( self, forward );

	// Trace from muzzle to see what we hit
	pounceRange = self->client->ps.weapon == WP_ALEVEL3 ? LEVEL3_POUNCE_RANGE :
	              LEVEL3_POUNCE_UPG_RANGE;
	G_WideTrace( &tr, self, muzzle, forward, pounceRange, LEVEL3_POUNCE_WIDTH,
	             LEVEL3_POUNCE_WIDTH, &traceEnt );

	if ( not TakesDamages( traceEnt ) )
	{
		return false;
	}

	timeMax = self->client->ps.weapon == WP_ALEVEL3 ? LEVEL3_POUNCE_TIME : LEVEL3_POUNCE_TIME_UPG;
	damage  = payload * LEVEL3_POUNCE_DMG / timeMax;

	self->client->pmext.pouncePayload = 0;

	traceEnt->Damage((float)damage, self, VEC2GLM( tr.endpos ), forward,
	                         DAMAGE_NO_LOCDAMAGE, MOD_LEVEL3_POUNCE);

	SendMeleeHitEvent( self, traceEnt, &tr );

	return true;
}

/*
======================================================================

LEVEL4

======================================================================
*/

void G_ChargeAttack( gentity_t *self, gentity_t *victim )
{
	if ( !self->client || self->client->ps.weaponCharge <= 0 ||
	     !( self->client->ps.stats[ STAT_STATE ] & SS_CHARGING ) ||
	     self->client->ps.weaponTime )
	{
		return;
	}

	if ( not TakesDamages( victim ) )
	{
		return;
	}

	glm::vec3 forward = VEC2GLM( victim->s.origin ) - VEC2GLM( self->s.origin );
	VectorNormalize( forward );

	// For buildables, track the last MAX_TRAMPLE_BUILDABLES_TRACKED buildables
	//  hit, and do not do damage if the current buildable is in that list
	//  in order to prevent dancing over stuff to kill it very quickly
	if ( !victim->client )
	{
		for ( int i = 0; i < MAX_TRAMPLE_BUILDABLES_TRACKED; i++ )
		{
			if ( self->client->trampleBuildablesHit[ i ] == victim->num() )
			{
				return;
			}
		}

		self->client->trampleBuildablesHit[
		  self->client->trampleBuildablesHitPos++ % MAX_TRAMPLE_BUILDABLES_TRACKED ] =
		    victim->num();
	}

	int damage = LEVEL4_TRAMPLE_DMG * self->client->ps.weaponCharge / LEVEL4_TRAMPLE_DURATION;

	victim->Damage( static_cast<float>( damage ), self, VEC2GLM( victim->s.origin ), forward, DAMAGE_NO_LOCDAMAGE, MOD_LEVEL4_TRAMPLE );

	SendMeleeHitEvent( self, victim, nullptr );

	self->client->ps.weaponTime += LEVEL4_TRAMPLE_REPEAT;
}

/*
======================================================================

GENERIC

======================================================================
*/

static meansOfDeath_t ModWeight( const gentity_t *self )
{
	return G_Team( self ) == TEAM_HUMANS ? MOD_WEIGHT_H : MOD_WEIGHT_A;
}

void G_ImpactAttack( gentity_t *self, gentity_t *victim )
{
	// self must be a client
	if ( !self->client )
	{
		return;
	}

	// don't do friendly fire
	if ( G_OnSameTeam( self, victim ) )
	{
		return;
	}

	// attacker must be above victim
	if ( self->client->ps.origin[ 2 ] + self->r.mins[ 2 ] <
	     victim->s.origin[ 2 ] + victim->r.maxs[ 2 ] )
	{
		return;
	}

	// allow the granger airlifting ritual
	if ( victim->client && victim->client->ps.stats[ STAT_STATE2 ] & SS2_JETPACK_ACTIVE &&
	     ( self->client->pers.classSelection == PCL_ALIEN_BUILDER0 ||
	       self->client->pers.classSelection == PCL_ALIEN_BUILDER0_UPG ) )
	{
		return;
	}

	// calculate impact damage
	float impactVelocity = fabs( self->client->pmext.fallImpactVelocity[ 2 ] ) * QU_TO_METER; // in m/s

	if (!impactVelocity) return;

	int attackerMass = BG_Class( self->client->pers.classSelection )->mass;
	float impactEnergy = attackerMass * impactVelocity * impactVelocity; // in J
	float impactDamage = impactEnergy * IMPACTDMG_JOULE_TO_DAMAGE;

	// calculate knockback direction
	glm::vec3 knockbackDir = VEC2GLM( victim->s.origin ) - VEC2GLM( self->client->ps.origin );
	VectorNormalize( knockbackDir );

	victim->Damage( impactDamage, self, VEC2GLM( victim->s.origin ), knockbackDir, DAMAGE_NO_LOCDAMAGE, ModWeight( self ) );
}

void G_WeightAttack( gentity_t *self, gentity_t *victim )
{
	float  weightDPS, weightDamage;
	int    attackerMass, victimMass;

	// weight damage is only dealt between clients
	if ( !self->client || !victim->client )
	{
		return;
	}

	// don't do friendly fire
	if ( G_OnSameTeam( self, victim ) )
	{
		return;
	}

	// attacker must be above victim
	if ( self->client->ps.origin[ 2 ] + self->r.mins[ 2 ] <
	     victim->s.origin[ 2 ] + victim->r.maxs[ 2 ] )
	{
		return;
	}

	// victim must be on the ground
	if ( victim->client->ps.groundEntityNum == ENTITYNUM_NONE )
	{
		return;
	}

	// check timer
	if ( victim->client->nextCrushTime > level.time )
	{
		return;
	}

	attackerMass = BG_Class( self->client->pers.classSelection )->mass;
	victimMass = BG_Class( victim->client->pers.classSelection )->mass;
	weightDPS = WEIGHTDMG_DMG_MODIFIER * std::max( attackerMass - victimMass, 0 );

	if ( weightDPS > WEIGHTDMG_DPS_THRESHOLD )
	{
		weightDamage = weightDPS * ( WEIGHTDMG_REPEAT / 1000.0f );

		victim->Damage(weightDamage, self, VEC2GLM( victim->s.origin ), Util::nullopt,
		                       DAMAGE_NO_LOCDAMAGE, ModWeight(self));
	}

	victim->client->nextCrushTime = level.time + WEIGHTDMG_REPEAT;
}

//======================================================================

/*
===============
Set muzzle location relative to pivoting eye.
===============
*/
void G_CalcMuzzlePoint( const gentity_t *self, const vec3_t forward, vec3_t muzzlePoint )
{
	vec3_t normal;

	VectorCopy( self->client->ps.origin, muzzlePoint );
	BG_GetClientNormal( &self->client->ps, normal );
	VectorMA( muzzlePoint, self->client->ps.viewheight, normal, muzzlePoint );
	VectorMA( muzzlePoint, 1, forward, muzzlePoint );
	// snap to integer coordinates for more efficient network bandwidth usage
	SnapVector( muzzlePoint );
}

void G_FireWeapon( gentity_t *self, weapon_t weapon, weaponMode_t weaponMode )
{
	switch ( weaponMode )
	{
		case WPM_PRIMARY:
		{
			switch ( weapon )
			{
				case WP_ALEVEL1:
				{
					gentity_t *target = FireMelee( self, LEVEL1_CLAW_RANGE, LEVEL1_CLAW_WIDTH, LEVEL1_CLAW_WIDTH,
					                               LEVEL1_CLAW_DMG, MOD_LEVEL1_CLAW, false );
					if ( target && target->client )
					{
						target->client->ps.stats[ STAT_STATE2 ] |= SS2_LEVEL1SLOW;
						target->client->lastLevel1SlowTime = level.time;
					}
				}
					break;

				case WP_ALEVEL3:
					FireMelee( self, LEVEL3_CLAW_RANGE, LEVEL3_CLAW_WIDTH, LEVEL3_CLAW_WIDTH,
					           LEVEL3_CLAW_DMG, MOD_LEVEL3_CLAW, false );
					break;

				case WP_ALEVEL3_UPG:
					FireMelee( self, LEVEL3_CLAW_UPG_RANGE, LEVEL3_CLAW_WIDTH, LEVEL3_CLAW_WIDTH,
					           LEVEL3_CLAW_DMG, MOD_LEVEL3_CLAW, false );
					break;

				case WP_ALEVEL2:
					FireMelee( self, LEVEL2_CLAW_RANGE, LEVEL2_CLAW_WIDTH, LEVEL2_CLAW_WIDTH,
					           LEVEL2_CLAW_DMG, MOD_LEVEL2_CLAW, false );
					break;

				case WP_ALEVEL2_UPG:
					FireMelee( self, LEVEL2_CLAW_U_RANGE, LEVEL2_CLAW_WIDTH, LEVEL2_CLAW_WIDTH,
					           LEVEL2_CLAW_DMG, MOD_LEVEL2_CLAW, false );
					break;

				case WP_ALEVEL4:
					FireMelee( self, LEVEL4_CLAW_RANGE, LEVEL4_CLAW_WIDTH, LEVEL4_CLAW_HEIGHT,
					           LEVEL4_CLAW_DMG, MOD_LEVEL4_CLAW, false );
					break;

				case WP_BLASTER:
					FireMissile( self, MIS_BLASTER );
					break;

				case WP_MACHINEGUN:
					FireBullet( self, RIFLE_SPREAD, (float)RIFLE_DMG, MOD_MACHINEGUN, false );
					break;

				case WP_SHOTGUN:
					FireShotgun( self );
					break;

				case WP_CHAINGUN:
					FireBullet( self, CHAINGUN_SPREAD, (float)CHAINGUN_DMG, MOD_CHAINGUN, false );
					break;

				case WP_FLAMER:
					FireMissile( self, MIS_FLAMER );
					break;

				case WP_PULSE_RIFLE:
					FireMissile( self, MIS_PRIFLE );
					break;

				case WP_MASS_DRIVER:
					FireBullet( self, 0.f, (float)MDRIVER_DMG, MOD_MDRIVER, DAMAGE_KNOCKBACK );
					break;

				case WP_LUCIFER_CANNON:
					FireLcannon( self, false );
					break;

				case WP_LAS_GUN:
					FireBullet( self, 0.f, LASGUN_DAMAGE, MOD_LASGUN, 0 );
					break;

				case WP_PAIN_SAW:
					FireMelee( self, PAINSAW_RANGE, PAINSAW_WIDTH, PAINSAW_HEIGHT, PAINSAW_DAMAGE, MOD_PAINSAW, true );
					break;

				case WP_MGTURRET:
					FireBullet( self, MGTURRET_SPREAD, self->turretCurrentDamage, MOD_MGTURRET, false );
					break;

				case WP_ABUILD:
				case WP_ABUILD2:
					FireBuild( self, MN_A_BUILD );
					break;

				case WP_HBUILD:
					FireBuild( self, MN_H_BUILD );
					break;

				default:
					break;
			}
			break;
		}
		case WPM_SECONDARY:
		{
			switch ( weapon )
			{
				case WP_LUCIFER_CANNON:
					FireLcannon( self, true );
					break;

				case WP_ALEVEL2_UPG:
					FireAreaZap( self );
					break;

				case WP_ABUILD:
				case WP_ABUILD2:
				case WP_HBUILD:
					CancelBuild( self );
					break;

				default:
					break;
			}
			break;
		}
		case WPM_TERTIARY:
		{
			switch ( weapon )
			{
				case WP_ALEVEL3_UPG:
					FireMissile( self, MIS_BOUNCEBALL );
					break;

				case WP_ABUILD2:
					FireMissile( self, MIS_SLOWBLOB );
					break;

				default:
					break;
			}
			break;
		}
		case WPM_DECONSTRUCT:
		{
			switch ( weapon )
			{
				case WP_ABUILD:
				case WP_ABUILD2:
				case WP_HBUILD:
					FireMarkDeconstruct( self );
					break;

				default:
					break;
			}
			break;
		}
		case WPM_DECONSTRUCT_SELECT_TARGET:
		{
			switch ( weapon )
			{
				case WP_ABUILD:
				case WP_ABUILD2:
				case WP_HBUILD:
					DeconstructSelectTarget( self );
					break;

				default:
					break;
			}
			break;
		}
		case WPM_DECONSTRUCT_LONG:
		{
			switch ( weapon )
			{
				case WP_ABUILD:
				case WP_ABUILD2:
				case WP_HBUILD:
					FireForceDeconstruct( self );
					break;

				default:
					break;
			}
			break;
		}
		default:
		{
			break;
		}
	}

}

void G_FireUpgrade( gentity_t *self, upgrade_t upgrade )
{
	if ( !self || !self->client )
	{
		Log::Warn( "G_FireUpgrade: Called with non-player parameter." );
		return;
	}

	if ( upgrade <= UP_NONE || upgrade >= UP_NUM_UPGRADES )
	{
		Log::Warn( "G_FireUpgrade: Called with unknown upgrade." );
		return;
	}

	switch ( upgrade )
	{
		case UP_GRENADE:
			FireMissile( self, MIS_GRENADE );
			break;
		case UP_FIREBOMB:
			FireMissile( self, MIS_FIREBOMB );
			break;
		default:
			break;
	}

	switch ( upgrade )
	{
		case UP_GRENADE:
		case UP_FIREBOMB:
			trap_SendServerCommand( self->num(), "vcommand grenade" );
			break;

		default:
			break;
	}
}
