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

// See https://wiki.unvanquished.net/wiki/Server/Map_rotation for user documentation

#include "common/Common.h"
#include "sg_local.h"
#include "common/FileSystem.h"

#define MAX_MAP_ROTATIONS     64
#define MAX_MAP_ROTATION_MAPS 256

#define NOT_ROTATING          -1

struct mrNode_t;
struct mrCondition_t
{
	mrNode_t            *target;
	char *exprString;
};

struct mrMapDescription_t
{
	char name[ MAX_QPATH ];

	char postCommand[ MAX_STRING_CHARS ];
	char layouts[ MAX_CVAR_VALUE_STRING ];
};

struct mrLabel_t
{
	char name[ MAX_QPATH ];
};

enum nodeType_t
{
  NT_MAP,
  NT_CONDITION,
  NT_GOTO,
  NT_RESUME,
  NT_LABEL,
  NT_RETURN
};

struct mrNode_t
{
	nodeType_t type;

	union
	{
		mrMapDescription_t  map;
		mrCondition_t       condition;
		mrLabel_t           label;
	} u;
};

struct mapRotation_t
{
	char   name[ MAX_QPATH ];

	mrNode_t *nodes[ MAX_MAP_ROTATION_MAPS ];
	int      numNodes;
	int      currentNode;
};

struct mapRotations_t
{
	mapRotation_t rotations[ MAX_MAP_ROTATIONS ];
	int           numRotations;
};

static mapRotations_t mapRotations;

static int            G_CurrentNodeIndex( int rotation );
static int            G_NodeIndexAfter( int currentNode, int rotation );

/*
===============
G_MapExists

Check if a map exists
===============
*/
bool G_MapExists( const char *name )
{
	// Due to filesystem changes, checking whether "maps/$name.bsp" exists in the
	// VFS is no longer the correct way to check whether a map exists
	// Legacy paks not supported here.
	return *name && FS::Path::BaseName( name ) == name && trap_FindPak( va( "map-%s", name ) );
}

/*
===============
G_RotationExists

Check if a rotation exists
===============
*/
static bool G_RotationExists( const char *name )
{
	int i;

	for ( i = 0; i < mapRotations.numRotations; i++ )
	{
		if ( Q_strncmp( mapRotations.rotations[ i ].name, name, MAX_QPATH ) == 0 )
		{
			return true;
		}
	}

	return false;
}

/*
===============
G_LabelExists

Check if a label exists in a rotation
===============
*/
static bool G_LabelExists( int rotation, const char *name )
{
	mapRotation_t *mr = &mapRotations.rotations[ rotation ];
	int           i;

	for ( i = 0; i < mr->numNodes; i++ )
	{
		mrNode_t *node = mr->nodes[ i ];

		if ( node->type == NT_LABEL &&
		     !Q_stricmp( name, node->u.label.name ) )
		{
			return true;
		}

		if ( node->type == NT_MAP &&
		     !Q_stricmp( name, node->u.map.name ) )
		{
			return true;
		}
	}

	return false;
}

/*
===============
G_AllocateNode

Allocate memory for a mrNode_t
===============
*/
static mrNode_t *G_AllocateNode()
{
	mrNode_t *node = (mrNode_t*) BG_Alloc( sizeof( mrNode_t ) );

	return node;
}

/*
===============
G_ParseMapCommandSection

Parse a map rotation command section
===============
*/
static bool G_ParseMapCommandSection( mrNode_t *node, const char **text_p )
{
	mrMapDescription_t *map = &node->u.map;
	int   commandLength = 0;

	// read optional parameters
	while ( 1 )
	{
		const char *token = COM_Parse( text_p );

		if ( !*token )
		{
			break;
		}

		if ( !Q_stricmp( token, "}" ) )
		{
			if ( commandLength > 0 )
			{
				// Replace last ; with \n
				map->postCommand[ commandLength - 1 ] = '\n';
			}

			return true; //reached the end of this command section
		}

		if ( !Q_stricmp( token, "layouts" ) )
		{
			token = COM_ParseExt( text_p, false );
			map->layouts[ 0 ] = '\0';

			while ( token[ 0 ] != 0 )
			{
				Q_strcat( map->layouts, sizeof( map->layouts ), token );
				Q_strcat( map->layouts, sizeof( map->layouts ), " " );
				token = COM_ParseExt( text_p, false );
			}

			continue;
		}

		// Parse the rest of the line into map->postCommand
		Q_strcat( map->postCommand, sizeof( map->postCommand ), token );
		Q_strcat( map->postCommand, sizeof( map->postCommand ), " " );

		token = COM_ParseExt( text_p, false );

		while ( token[ 0 ] != 0 )
		{
			Q_strcat( map->postCommand, sizeof( map->postCommand ), token );
			Q_strcat( map->postCommand, sizeof( map->postCommand ), " " );
			token = COM_ParseExt( text_p, false );
		}

		commandLength = strlen( map->postCommand );
		ASSERT_GE(commandLength, 1);
		map->postCommand[ commandLength - 1 ] = ';';
	}

	return false;
}

using mrValue_t = float; // value type for expressions in 'if' conditions

// upper nibble is precedence
// operators and their precedences are the same as C-like languages
enum mrOperation_t {
	OP_LEFT_PAREN = 0x10,

	OP_OR = 0x20,

	OP_AND = 0x30,

	OP_EQUAL = 0x40,
	OP_UNEQUAL = 0x41,

	OP_LESS = 0x50,
	OP_LESS_EQUAL = 0x51,
	OP_GREATER = 0x52,
	OP_GREATER_EQUAL = 0x53,

	OP_ADD = 0x60,
	OP_SUBTRACT = 0x61,

	OP_MULTIPLY = 0x70,
	OP_DIVIDE = 0x71,
	OP_MODULO = 0x72,

	// unary operators
	OP_MINUS = 0x80,
	OP_NOT = 0x81,
	OP_LAST_WIN = 0x82,
};

static int Precedence( mrOperation_t op )
{
	return op >> 4;
}

static bool ParseValue( const char *token, mrValue_t &result )
{
	if ( Str::ToFloat( token, result ) )
	{
		return true;
	}

	if ( !Q_stricmp( token, "numPlayers" ) )
	{
		result = level.numConnectedPlayers;
	}
	else if ( !Q_stricmp( token, "numClients" ) )
	{
		result = level.numConnectedClients;
	}
	else if ( !Q_stricmp( token, "aliens" ) )
	{
		result = TEAM_ALIENS;
	}
	else if ( !Q_stricmp( token, "humans" ) )
	{
		result = TEAM_HUMANS;
	}
	else if ( !Q_stricmp( token, "random" ) )
	{
		result = rand() & 1;
	}
	else if ( !Q_stricmp( token, "time" ) ) // 0000 to 2359
	{
		// We use UTC time since localtime doesn't work in NaCl and maprotations would mostly
		// be used on hosted servers which are normally configured to UTC anyway.
		qtime_t t;
		Com_GMTime( &t );
		result = t.tm_hour * 100 + t.tm_min;
	}
	else if ( !Q_stricmp( token, "day" ) ) // 1 to 31
	{
		qtime_t t;
		Com_GMTime( &t );
		result = t.tm_mday;
	}
	else if ( !Q_stricmp( token, "month" ) ) // 1 to 12
	{
		qtime_t t;
		Com_GMTime( &t );
		result = t.tm_mon + 1;
	}
	else if ( !Q_stricmp( token, "year" ) )
	{
		qtime_t t;
		Com_GMTime( &t );
		result = 1900 + t.tm_year;
	}
	else if ( !Q_stricmp( token, "weekday" ) ) // 1 (Sunday) to 7 (Saturday)
	{
		qtime_t t;
		Com_GMTime( &t );
		result = t.tm_wday + 1;
	}
	else
	{
		return false;
	}
	return true;
}

static mrValue_t EvalUnary( mrOperation_t op, mrValue_t operand )
{
	switch ( op )
	{
	case OP_MINUS:
		return -operand;
	case OP_NOT:
		return !operand;
	case OP_LAST_WIN:
		return operand == level.lastWin;
	default:
		ASSERT_UNREACHABLE();
	}
}

static mrValue_t EvalUnaries( std::vector<mrOperation_t> &stack, mrValue_t value )
{
	while ( Precedence( stack.back() ) == Precedence( OP_MINUS ) )
	{
		value = EvalUnary( stack.back(), value );
		stack.pop_back();
	}
	return value;
}

static mrValue_t EvalBinary( mrValue_t lhs, mrOperation_t op, mrValue_t rhs )
{
	switch ( op )
	{
	case OP_OR:
		return lhs || rhs;
	case OP_AND:
		return lhs && rhs;
	case OP_EQUAL:
		return lhs == rhs;
	case OP_UNEQUAL:
		return lhs != rhs;
	case OP_LESS:
		return lhs < rhs;
	case OP_LESS_EQUAL:
		return lhs <= rhs;
	case OP_GREATER:
		return lhs > rhs;
	case OP_GREATER_EQUAL:
		return lhs >= rhs;
	case OP_ADD:
		return lhs + rhs;
	case OP_SUBTRACT:
		return lhs - rhs;
	case OP_MULTIPLY:
		return lhs * rhs;
	case OP_DIVIDE:
		return lhs / rhs;
	case OP_MODULO:
		return fmodf( lhs, rhs );
	default:
		ASSERT_UNREACHABLE();
	}
}

static void EvalBinaries( std::vector<mrOperation_t> &opStack, std::vector<mrValue_t> &valueStack, int minPrecedence )
{
	while ( Precedence( opStack.back() ) >= minPrecedence )
	{
		mrValue_t rhs = valueStack.back();
		valueStack.pop_back();
		valueStack.back() = EvalBinary( valueStack.back(), opStack.back(), rhs );
		opStack.pop_back();
	}
}

static bool ParseUnary( const char *token, mrOperation_t &result )
{
	if ( !strcmp( token, "-" ) )
	{
		result = OP_MINUS;
	}
	else if ( !strcmp( token, "!" ) )
	{
		result = OP_NOT;
	}
	else if ( !Q_stricmp( token, "lastWin" ) )
	{
		result = OP_LAST_WIN;
	}
	else
	{
		return false;
	}
	return true;
}

static bool ParseBinary( const char *token, mrOperation_t &result )
{
	if ( !strcmp( token, "||" ) )
	{
		result = OP_OR;
	}
	else if ( !strcmp( token, "&&" ) )
	{
		result = OP_AND;
	}
	else if ( !strcmp( token, "=" ) || !strcmp( token, "==" ) )
	{
		result = OP_EQUAL;
	}
	else if ( !strcmp( token, "!=" ) )
	{
		result = OP_UNEQUAL;
	}
	else if ( !strcmp( token, "<" ) )
	{
		result = OP_LESS;
	}
	else if ( !strcmp( token, "<=" ) )
	{
		result = OP_LESS_EQUAL;
	}
	else if ( !strcmp( token, ">" ) )
	{
		result = OP_GREATER;
	}
	else if ( !strcmp( token, ">=" ) )
	{
		result = OP_GREATER_EQUAL;
	}
	else if ( !strcmp( token, "+" ) )
	{
		result = OP_ADD;
	}
	else if ( !strcmp( token, "-" ) )
	{
		result = OP_SUBTRACT;
	}
	else if ( !strcmp( token, "*" ) )
	{
		result = OP_MULTIPLY;
	}
	else if ( !strcmp( token, "/" ) )
	{
		result = OP_DIVIDE;
	}
	else if ( !strcmp( token, "%" ) )
	{
		result = OP_MODULO;
	}
	else
	{
		return false;
	}
	return true;
}

// Shunting yard parser for the expression of an 'if' node
static bool ParseExpression( const char **text_p, mrValue_t &result, std::string &str, const char*& lastToken )
{
	std::vector<mrOperation_t> opStack{ OP_LEFT_PAREN };
	std::vector<mrValue_t> valueStack;
	bool expectValue = true;
	int parenLevel = 0;
	std::string error;
	str.clear();
	const char *token;
	while ( true )
	{
		// COM_Parse2 sort of knows C tokens although it has a lot of deficiencies like
		// lumping '/' '-' and '*' with alnum strings
		token = COM_Parse2( text_p );
		if ( !*text_p )
		{
			if ( expectValue || parenLevel > 0 )
			{
				error = "Unexpected end of file";
			}
			break;
		}
		if ( expectValue )
		{
			mrOperation_t unary;
			if ( ParseUnary( token, unary ) )
			{
				opStack.push_back( unary );
			}
			else if ( !strcmp( token, "(" ) )
			{
				opStack.push_back( OP_LEFT_PAREN );
				++parenLevel;
			}
			else
			{
				expectValue = false;
				mrValue_t value;
				if ( !ParseValue( token, value ) )
				{
					error = Str::Format( "Could not parse '%s' as a value", token );
					break;
				}
				valueStack.push_back( EvalUnaries( opStack, value ) );
			}
		}
		else
		{
			if ( !strcmp( token, ")" ) )
			{
				if ( --parenLevel < 0 )
				{
					error = "Closing ')' without opening '('";
					break;
				}
				EvalBinaries( opStack, valueStack, Precedence( OP_LEFT_PAREN ) + 1 );
				ASSERT_EQ( opStack.back(), OP_LEFT_PAREN );
				opStack.pop_back();
				valueStack.back() = EvalUnaries( opStack, valueStack.back() );
			}
			else
			{
				expectValue = true;
				mrOperation_t binaryOp;
				if ( !ParseBinary( token, binaryOp ) )
				{
					if ( parenLevel > 0 )
					{
						error = Str::Format( "Got '%s'; expected binary operator or ')'", token );
						break;
					}
					else
					{
						// assume this is the target of the condition
						if ( !strcmp( token, "#" ) )
						{
							// HACK: we have to back up because COM_Parse considers #label (implicit goto) to be one token
							--*text_p;
							token = COM_Parse( text_p );
						}
						break;
					}
					break;
				}
				EvalBinaries( opStack, valueStack, Precedence( binaryOp ) );
				opStack.push_back( binaryOp );
			}
		}
		if ( !str.empty() ) str.push_back( ' ' );
		str += token;
	}

	if ( !error.empty() )
	{
		COM_ParseError( "While parsing 'if' expression: %s", error.c_str() );
		return false;
	}
	else
	{
		lastToken = token;
		EvalBinaries( opStack, valueStack, Precedence( OP_LEFT_PAREN ) + 1 );
		ASSERT_EQ( opStack.size(), 1U ); // dummy OP_LEFT_PAREN
		ASSERT_EQ( valueStack.size(), 1U );
		result = valueStack.back();
		return true;
	}
}

/*
===============
G_ParseNode

Parse a node
===============
*/
static bool G_ParseNode( mrNode_t **node, const char *token, const char **text_p, bool conditional )
{
	if ( !Q_stricmp( token, "if" ) )
	{
		mrValue_t unusedResult;
		std::string str;
		if ( !ParseExpression( text_p, unusedResult, str, token ) )
		{
			return false;
		}

		if ( !*text_p )
		{
			COM_ParseError( "Unexpected EOF after 'if' condition" );
			return false;
		}

		(*node)->type = NT_CONDITION;
		mrCondition_t &condition = (*node)->u.condition;
		condition.exprString = BG_strdup( str.c_str() );
		condition.target = G_AllocateNode();
		*node = condition.target;

		return G_ParseNode( node, token, text_p, true );
	}
	else if ( !Q_stricmp( token, "return" ) )
	{
		( *node )->type = NT_RETURN;
	}
	else if ( !Q_stricmp( token, "goto" ) ||
	          !Q_stricmp( token, "resume" ) )
	{
		mrLabel_t *label;

		if ( !Q_stricmp( token, "goto" ) )
		{
			( *node )->type = NT_GOTO;
		}
		else
		{
			( *node )->type = NT_RESUME;
		}

		label = & ( *node )->u.label;

		token = COM_Parse( text_p );

		if ( !*token )
		{
			COM_ParseError("goto or resume without label" );
			return false;
		}

		Q_strncpyz( label->name, token, sizeof( label->name ) );
	}
	else if ( *token == '#' )
	{
		mrLabel_t *label;

		( *node )->type = ( conditional ) ? NT_GOTO : NT_LABEL;
		label = & ( *node )->u.label;

		Q_strncpyz( label->name, token, sizeof( label->name ) );
	}
	else
	{
		if ( !FS::Path::IsValid( token, false ) )
		{
			COM_ParseError( "invalid token '%s'", token );
			return false;
		}

		mrMapDescription_t *map;

		( *node )->type = NT_MAP;
		map = & ( *node )->u.map;

		Q_strncpyz( map->name, token, sizeof( map->name ) );
		map->postCommand[ 0 ] = '\0';
	}

	return true;
}

/*
===============
G_ParseMapRotation

Parse a map rotation section
===============
*/
static bool G_ParseMapRotation( mapRotation_t *mr, const char **text_p )
{
	mrNode_t *node = nullptr;

	// read optional parameters
	while ( 1 )
	{
		const char *token = COM_Parse( text_p );

		if ( !*token )
		{
			break;
		}

		if ( !Q_stricmp( token, "{" ) )
		{
			if ( node == nullptr )
			{
				COM_ParseError("map command section with no associated map" );
				return false;
			}

			if ( !G_ParseMapCommandSection( node, text_p ) )
			{
				COM_ParseError("failed to parse map command section" );
				return false;
			}

			continue;
		}
		else if ( !Q_stricmp( token, "}" ) )
		{
			// Reached the end of this map rotation
			return true;
		}

		if ( mr->numNodes == MAX_MAP_ROTATION_MAPS )
		{
			Log::Warn("maximum number of maps in one rotation (%d) reached",
			          MAX_MAP_ROTATION_MAPS );
			return false;
		}

		node = G_AllocateNode();
		mr->nodes[ mr->numNodes++ ] = node;

		if ( !G_ParseNode( &node, token, text_p, false ) )
		{
			return false;
		}
	}

	return false;
}

/*
===============
G_ParseMapRotationFile

Load the map rotations from a map rotation file
===============
*/
static bool G_ParseMapRotationFile( const char *fileName )
{
	const char *text_p;
	int          i, j;
	int          len;
	char         text[ 20000 ];
	char         mrName[ MAX_QPATH ];
	bool     mrNameSet = false;
	fileHandle_t f;

	// load the file
	len = BG_FOpenGameOrPakPath( fileName, f );

	if ( len < 0 )
	{
		Log::Warn( "file '%s' not found", fileName );
		return false;
	}

	if ( len == 0 || len >= (int) sizeof( text ) - 1 )
	{
		trap_FS_FCloseFile( f );
		Log::Warn("map rotation file %s is %s", fileName,
		          len == 0 ? "empty" : "too long" );
		return false;
	}

	trap_FS_Read( text, len, f );
	text[ len ] = 0;
	trap_FS_FCloseFile( f );

	// parse the text
	text_p = text;

	COM_BeginParseSession( fileName );

	// read optional parameters
	while ( 1 )
	{
		const char *token = COM_Parse( &text_p );

		if ( !*token )
		{
			break;
		}

		if ( !Q_stricmp( token, "{" ) )
		{
			if ( mrNameSet )
			{
				//check for name space clashes
				if ( G_RotationExists( mrName ) )
				{
					COM_ParseError("a map rotation is already named %s", mrName );
					return false;
				}

				if ( mapRotations.numRotations == MAX_MAP_ROTATIONS )
				{
					Log::Warn("maximum number of map rotations (%d) reached",
					          MAX_MAP_ROTATIONS );
					return false;
				}

				Q_strncpyz( mapRotations.rotations[ mapRotations.numRotations ].name, mrName, MAX_QPATH );

				if ( !G_ParseMapRotation( &mapRotations.rotations[ mapRotations.numRotations ], &text_p ) )
				{
					Log::Warn("%s: failed to parse map rotation %s", fileName, mrName );
					return false;
				}

				mapRotations.numRotations++;

				//start parsing map rotations again
				mrNameSet = false;

				continue;
			}
			else
			{
				COM_ParseError("unnamed map rotation" );
				return false;
			}
		}

		if ( !mrNameSet )
		{
			Q_strncpyz( mrName, token, sizeof( mrName ) );
			mrNameSet = true;
		}
		else
		{
			COM_ParseError("map rotation already named" );
			return false;
		}
	}

	for ( i = 0; i < mapRotations.numRotations; i++ )
	{
		mapRotation_t *mr = &mapRotations.rotations[ i ];
		bool empty = true;

		for ( j = 0; j < mr->numNodes; j++ )
		{
			mrNode_t *node = mr->nodes[ j ];

			if ( node->type == NT_MAP )
			{
				empty = false;

				if ( !G_MapExists( node->u.map.name ) )
				{
					Log::Warn("rotation map \"%s\" doesn't exist",
					          node->u.map.name );
					return false;
				}

				continue;
			}
			else if ( node->type == NT_RETURN )
			{
				continue;
			}
			else if ( node->type == NT_LABEL )
			{
				continue;
			}
			else
			{
				while ( node->type == NT_CONDITION )
				{
					node = node->u.condition.target;
				}
			}

			if ( node->type == NT_GOTO || node->type == NT_RESUME )
			{
				if ( G_RotationExists( node->u.label.name ) )
				{
					empty = false;
				}
				else if ( !G_LabelExists( i, node->u.label.name ) )
				{
					Log::Warn( "goto destination named \"%s\" doesn't exist", node->u.label.name );
					return false;
				}
			}
		}

		if ( empty )
		{
			Log::Warn("rotation \"%s\" has no maps",
			          mr->name );
			return false;
		}
	}

	return true;
}

// Some constants for map rotation listing
#define MAP_BAD            "^1"
#define MAP_CURRENT        "^2"
#define MAP_CONTROL        "^5"
#define MAP_DEFAULT        "^7"
#define MAP_CURRENT_MARKER "‣"

/*
===============
G_RotationNode_ToString
===============
*/
static const char *G_RotationNode_ToString( const mrNode_t *node )
{
	switch ( node->type )
	{
		case NT_MAP:
			return node->u.map.name;

		case NT_CONDITION:
			return va( MAP_CONTROL "condition: %s", node->u.condition.exprString );
			break;

		case NT_GOTO:
			return va( MAP_CONTROL "goto: %s", node->u.label.name );

		case NT_RESUME:
			return va( MAP_CONTROL "resume: %s", node->u.label.name );

		case NT_LABEL:
			return va( MAP_CONTROL "label: %s", node->u.label.name );

		case NT_RETURN:
			return MAP_CONTROL "return";

		default:
			return MAP_BAD "???";
	}
}

/*
===============
G_PrintRotations

Print the parsed map rotations
===============
*/
void G_PrintRotations()
{
	int i, j;

	Log::Notice( "Map rotations as parsed:" );

	for ( i = 0; i < mapRotations.numRotations; i++ )
	{
		mapRotation_t *mr = &mapRotations.rotations[ i ];

		Log::Notice( "rotation: %s{", mr->name );

		for ( j = 0; j < mr->numNodes; j++ )
		{
			mrNode_t *node = mr->nodes[ j ];
			int    indentation = 2;

			while ( node->type == NT_CONDITION )
			{
				Log::Notice( "%*s%s", indentation, "", G_RotationNode_ToString( node ) );
				node = node->u.condition.target;

				indentation += 2;
			}

			Log::Notice( "%*s%s", indentation, "", G_RotationNode_ToString( node ) );

			if ( node->type == NT_MAP && strlen( node->u.map.postCommand ) > 0 )
			{
				Log::Notice( MAP_CONTROL "    command: %s", node->u.map.postCommand ); // assume that there's an LF there... if not, well...
			}

		}

		Log::Notice( MAP_DEFAULT "}" );
	}
}

/*
===============
G_PrintCurrentRotation

Print the current rotation to an entity
===============
*/
void G_PrintCurrentRotation( gentity_t *ent )
{
	int           mapRotationIndex = g_currentMapRotation.Get();
	mapRotation_t *mapRotation = G_MapRotationActive() ? &mapRotations.rotations[ mapRotationIndex ] : nullptr;
	int           i = 0;
	char          currentMapName[ MAX_QPATH ];
	bool      currentShown = false;
	mrNode_t        *node;

	if ( mapRotation == nullptr )
	{
		trap_SendServerCommand( ent->num(), "print_tr " QQ( N_("^3listrotation:^* there is no active map rotation on this server") ) );
		return;
	}

	if ( mapRotation->numNodes == 0 )
	{
		trap_SendServerCommand( ent->num(), "print_tr " QQ( N_("^3listrotation:^* there are no maps in the active map rotation") ) );
		return;
	}

	trap_Cvar_VariableStringBuffer( "mapname", currentMapName, sizeof( currentMapName ) );

	ADMBP_begin();
	ADMBP( va( "%s:", mapRotation->name ) );

	while ( ( node = mapRotation->nodes[ i++ ] ) )
	{
		const char *colour = MAP_DEFAULT;
		int         indentation = 7;
		bool    currentMap = false;
		bool    override = false;

		if ( node->type == NT_MAP && !G_MapExists( node->u.map.name ) )
		{
			colour = MAP_BAD;
		}
		else if ( G_NodeIndexAfter( i - 1, mapRotationIndex ) == G_CurrentNodeIndex( mapRotationIndex ) )
		{
			currentMap = true;
			currentShown = node->type == NT_MAP;
			override = currentShown && Q_stricmp( node->u.map.name, currentMapName );

			if ( !override )
			{
				colour = MAP_CURRENT;
			}
		}

		ADMBP( va( "^7%s%3i %s%s",
		           ( currentMap && currentShown && !override ) ? MAP_CURRENT_MARKER : " ",
		           i, colour, G_RotationNode_ToString( node ) ) );

		while ( node->type == NT_CONDITION )
		{
			node = node->u.condition.target;
			ADMBP( va( "%*s%s%s", indentation, "", colour, G_RotationNode_ToString( node ) ) );
			indentation += 2;
		}

		if ( override )
		{
			ADMBP( va( MAP_DEFAULT MAP_CURRENT_MARKER "    " MAP_CURRENT "%s", currentMapName ) ); // use current map colour here
		}
		if ( currentMap && currentShown && G_MapExists( g_nextMap.Get().c_str() ) )
		{
			ADMBP( va( MAP_DEFAULT "     %s", g_nextMap.Get().c_str() ) );
			currentMap = false;
		}
	}

	// current map will not have been shown if we're at the last entry
	// (e.g. server just started up) and that entry is not for a map
	if ( !currentShown )
	{
		ADMBP( va( MAP_DEFAULT MAP_CURRENT_MARKER "    " MAP_CURRENT "%s", currentMapName ) ); // use current map colour here

		if ( G_MapExists( g_nextMap.Get().c_str() ) )
		{
			ADMBP( va( MAP_DEFAULT "     %s", g_nextMap.Get().c_str() ) );
		}
	}


	ADMBP_end();
}

/*
===============
G_ClearRotationStack

Clear the rotation stack
===============
*/
void G_ClearRotationStack()
{
	g_mapRotationStack.Set("");
}

/*
===============
G_PushRotationStack

Push the rotation stack
===============
*/
static void G_PushRotationStack( int rotation )
{
	char text[ MAX_CVAR_VALUE_STRING ];

	Com_sprintf( text, sizeof( text ), "%d %s",
	             rotation, g_mapRotationStack.Get().c_str() );
	g_mapRotationStack.Set(text);
}

/*
===============
G_PopRotationStack

Pop the rotation stack
===============
*/
static int G_PopRotationStack()
{
	int  value = -1;
	char text[ MAX_CVAR_VALUE_STRING ];
	const char *text_p, *token;

	Q_strncpyz( text, g_mapRotationStack.Get().c_str(), sizeof( text ) );

	text_p = text;
	token = COM_Parse( &text_p );

	if ( *token )
	{
		value = atoi( token );
	}

	if ( text_p )
	{
		while ( *text_p == ' ' )
		{
			text_p++;
		}

		g_mapRotationStack.Set(text_p);
	}
	else
	{
		G_ClearRotationStack();
	}

	return value;
}

/*
===============
G_RotationNameByIndex

Returns the name of a rotation by its index
===============
*/
static char *G_RotationNameByIndex( int index )
{
	if ( index >= 0 && index < mapRotations.numRotations )
	{
		return mapRotations.rotations[ index ].name;
	}

	return nullptr;
}

/*
===============
G_CurrentNodeIndexArray

Fill a static array with the current node of each rotation
===============
*/
static int *G_CurrentNodeIndexArray()
{
	static int currentNode[ MAX_MAP_ROTATIONS ];
	int        i = 0;
	char       text[ MAX_MAP_ROTATIONS * 2 ];
	const char       *text_p, *token;

	Q_strncpyz( text, g_mapRotationNodes.Get().c_str(), sizeof( text ) );

	text_p = text;

	while ( 1 )
	{
		token = COM_Parse( &text_p );

		if ( !*token )
		{
			break;
		}

		currentNode[ i++ ] = atoi( token );
	}

	return currentNode;
}

/*
===============
G_SetCurrentNodeByIndex

Set the current map in some rotation
===============
*/
static void G_SetCurrentNodeByIndex( int currentNode, int rotation )
{
	char text[ MAX_MAP_ROTATIONS * 4 ] = { 0 };
	int  *p = G_CurrentNodeIndexArray();
	int  i;

	p[ rotation ] = currentNode;

	for ( i = 0; i < mapRotations.numRotations; i++ )
	{
		Q_strcat( text, sizeof( text ), va( "%d ", p[ i ] ) );
	}

	g_mapRotationNodes.Set(text);
}

/*
===============
G_CurrentNodeIndex

Return the current node index in some rotation
===============
*/
static int G_CurrentNodeIndex( int rotation )
{
	int *p = G_CurrentNodeIndexArray();

	return p[ rotation ];
}

/*
===============
G_NodeByIndex

Return a node in a rotation by its index
===============
*/
static mrNode_t *G_NodeByIndex( int index, int rotation )
{
	if ( rotation >= 0 && rotation < mapRotations.numRotations &&
	     index >= 0 && index < mapRotations.rotations[ rotation ].numNodes )
	{
		return mapRotations.rotations[ rotation ].nodes[ index ];
	}

	return nullptr;
}

/*
===============
G_IssueMapChange

Send commands to the server to actually change the map
===============
*/
static void G_IssueMapChange( int index, int rotation )
{
	mrNode_t *node = mapRotations.rotations[ rotation ].nodes[ index ];

	if( node->type == NT_CONDITION )
	{
		mrCondition_t *condition;
		condition = &node->u.condition;
		node = condition->target;
	}

	mrMapDescription_t  *map = &node->u.map;

	if ( strlen( map->postCommand ) > 0 )
	{
		trap_SendConsoleCommand( map->postCommand );
	}

	char currentMapName[ MAX_STRING_CHARS ];
	trap_Cvar_VariableStringBuffer( "mapname", currentMapName, sizeof( currentMapName ) );

	// Restart if map is the same
	if ( !Q_stricmp( currentMapName, map->name ) )
	{
		// Set layout if it exists
		if ( g_layouts.Get().empty() && map->layouts[ 0 ] )
		{
			g_layouts.Set(map->layouts);
		}

		trap_SendConsoleCommand( "map_restart" );


	}
	// Load new map if different
	else
	{
		// allow a manually defined g_layouts setting to override the maprotation
		if ( g_layouts.Get().empty() && map->layouts[ 0 ] )
		{
			trap_SendConsoleCommand( Str::Format( "%s %s %s", G_NextMapCommand(), Cmd::Escape( map->name ), Cmd::Escape( map->layouts ) ).c_str() );
		}
		else
		{
			trap_SendConsoleCommand( Str::Format( "%s %s", G_NextMapCommand(), Cmd::Escape( map->name ) ).c_str() );
		}
	}
}

/*
===============
G_GotoLabel

Resolve the label of some condition
===============
*/
static bool G_GotoLabel( int rotation, int nodeIndex, char *name,
                             bool reset_index, int depth )
{
	mrNode_t *node;
	int    i;

	// Search the rotation names...
	if ( G_StartMapRotation( name, true, true, reset_index, depth ) )
	{
		return true;
	}

	// ...then try labels in the rotation
	for ( i = 0; i < mapRotations.rotations[ rotation ].numNodes; i++ )
	{
		node = mapRotations.rotations[ rotation ].nodes[ i ];

		if ( node->type == NT_LABEL && !Q_stricmp( node->u.label.name, name ) )
		{
			G_SetCurrentNodeByIndex( G_NodeIndexAfter( i, rotation ), rotation );
			G_AdvanceMapRotation( depth );
			return true;
		}
	}

	// finally check for a map by name
	for ( i = 0; i < mapRotations.rotations[ rotation ].numNodes; i++ )
	{
		nodeIndex = G_NodeIndexAfter( nodeIndex, rotation );
		node = mapRotations.rotations[ rotation ].nodes[ nodeIndex ];

		if ( node->type == NT_MAP && !Q_stricmp( node->u.map.name, name ) )
		{
			G_SetCurrentNodeByIndex( nodeIndex, rotation );
			G_AdvanceMapRotation( depth );
			return true;
		}
	}

	return false;
}

/*
===============
G_EvaluateMapCondition

Evaluate a map condition
===============
*/
static bool G_EvaluateMapCondition( mrCondition_t **condition )
{
	mrCondition_t *localCondition = *condition;
	mrValue_t result;
	std::string unused;
	const char* text_p = localCondition->exprString;
	const char* nextToken;
	bool parsed = ParseExpression( &text_p, result, unused, nextToken );
	if ( !parsed || *nextToken )
	{
		Sys::Drop( "Unexpected map rotation condition parsing failure" );
	}

	if ( localCondition->target->type == NT_CONDITION )
	{
		*condition = &localCondition->target->u.condition;

		return !!result && G_EvaluateMapCondition( condition );
	}

	return !!result;
}

/*
===============
G_NodeIndexAfter
===============
*/
static int G_NodeIndexAfter( int currentNode, int rotation )
{
	mapRotation_t *mr = &mapRotations.rotations[ rotation ];

	return ( currentNode + 1 ) % mr->numNodes;
}

/*
===============
G_StepMapRotation

Run one node of a map rotation
===============
*/
static bool G_StepMapRotation( int rotation, int nodeIndex, int depth )
{
	mrNode_t      *node;
	mrCondition_t *condition;
	int         returnRotation;
	bool    step = true;

	node = G_NodeByIndex( nodeIndex, rotation );
	depth++;

	// guard against infinite loop in conditional code
	if ( depth > 32 && node->type != NT_MAP )
	{
		if ( depth > 64 )
		{
			Log::Warn("infinite loop protection stopped at map rotation %s",
			          G_RotationNameByIndex( rotation ) );
			return false;
		}

		Log::Warn("possible infinite loop in map rotation %s",
		          G_RotationNameByIndex( rotation ) );
		return true;
	}

	while ( step )
	{
		step = false;

		switch ( node->type )
		{
			case NT_CONDITION:
				condition = &node->u.condition;

				if ( G_EvaluateMapCondition( &condition ) )
				{
					node = condition->target;
					step = true;
					continue;
				}

				break;

			case NT_RETURN:
				returnRotation = G_PopRotationStack();

				if ( returnRotation >= 0 )
				{
					G_SetCurrentNodeByIndex(
					  G_NodeIndexAfter( nodeIndex, rotation ), rotation );

					if ( G_StartMapRotation( G_RotationNameByIndex( returnRotation ),
					                         true, false, false, depth ) )
					{
						return false;
					}
				}

				break;

			case NT_MAP:
				if ( G_MapExists( node->u.map.name ) )
				{
					G_SetCurrentNodeByIndex(
					  G_NodeIndexAfter( nodeIndex, rotation ), rotation );

					if ( !G_MapExists( g_nextMap.Get().c_str() ) )
					{
						G_IssueMapChange( nodeIndex, rotation );
					}

					return false;
				}

				Log::Warn("skipped missing map %s in rotation %s",
				          node->u.map.name, G_RotationNameByIndex( rotation ) );
				break;

			case NT_LABEL:
				break;

			case NT_GOTO:
			case NT_RESUME:
				G_SetCurrentNodeByIndex(
				  G_NodeIndexAfter( nodeIndex, rotation ), rotation );

				if ( G_GotoLabel( rotation, nodeIndex, node->u.label.name,
				                  ( node->type == NT_GOTO ), depth ) )
				{
					return false;
				}

				Log::Warn("label, map, or rotation %s not found in %s",
				          node->u.label.name, G_RotationNameByIndex( rotation ) );
				break;
		}
	}

	return true;
}

/*
===============
G_AdvanceMapRotation

Increment the current map rotation
===============
*/
void G_AdvanceMapRotation( int depth )
{
	mrNode_t *node;
	int    rotation;
	int    nodeIndex;

	rotation = g_currentMapRotation.Get();

	if ( rotation < 0 || rotation >= MAX_MAP_ROTATIONS )
	{
		return;
	}

	nodeIndex = G_CurrentNodeIndex( rotation );
	node = G_NodeByIndex( nodeIndex, rotation );

	if ( !node )
	{
		Log::Warn("index incorrect for map rotation %s, trying 0",
		          G_RotationNameByIndex( rotation ) );
		nodeIndex = 0;
		node = G_NodeByIndex( nodeIndex, rotation );
	}

	while ( node && G_StepMapRotation( rotation, nodeIndex, depth ) )
	{
		nodeIndex = G_NodeIndexAfter( nodeIndex, rotation );
		node = G_NodeByIndex( nodeIndex, rotation );
		depth++;
	}

	if ( !node )
	{
		Log::Warn("unexpected end of maprotation '%s'",
		          G_RotationNameByIndex( rotation ) );
	}
}

/*
===============
G_StartMapRotation

Switch to a new map rotation
===============
*/
bool G_StartMapRotation( const char *name, bool advance,
                             bool putOnStack, bool reset_index, int depth )
{
	int i;
	int currentRotation = g_currentMapRotation.Get();

	for ( i = 0; i < mapRotations.numRotations; i++ )
	{
		if ( !Q_stricmp( mapRotations.rotations[ i ].name, name ) )
		{
			if ( putOnStack && currentRotation >= 0 )
			{
				G_PushRotationStack( currentRotation );
			}

			g_currentMapRotation.Set( i );

			if ( advance )
			{
				if ( reset_index )
				{
					G_SetCurrentNodeByIndex( 0, i );
				}

				G_AdvanceMapRotation( depth );
			}

			break;
		}
	}

	if ( i == mapRotations.numRotations )
	{
		return false;
	}
	else
	{
		return true;
	}
}

/*
===============
G_StopMapRotation

Stop the current map rotation
===============
*/
void G_StopMapRotation()
{
	g_currentMapRotation.Set( NOT_ROTATING );
}

/*
===============
G_MapRotationActive

Test if any map rotation is currently active
===============
*/
bool G_MapRotationActive()
{
	return ( g_currentMapRotation.Get() > NOT_ROTATING && g_currentMapRotation.Get() <= MAX_MAP_ROTATIONS );
}

/*
===============
G_LoadMaprotation

Load a maprotation file if it exists
===============
*/
static void G_LoadMaprotation( const char *fileName )
{
	if ( !G_ParseMapRotationFile( fileName ) )
	{
		Log::Warn( "failed to load map rotation '%s'", fileName );
	}
}

/*
===============
G_InitMapRotations

Load and initialise the map rotations
===============
*/
void G_InitMapRotations()
{
	G_LoadMaprotation( "default/maprotation.cfg" );
	G_LoadMaprotation( "maprotation.cfg" );

	if ( g_currentMapRotation.Get() == NOT_ROTATING )
	{
		if ( !g_initialMapRotation.Get().empty() )
		{
			if( !G_StartMapRotation( g_initialMapRotation.Get().c_str(), false, true, false, 0 ) )
			{
				Log::Warn( "failed to load g_initialMapRotation: %s", g_initialMapRotation.Get() );
				G_StartMapRotation( "defaultRotation", false, true, false, 0 );
			}

			g_initialMapRotation.Set("");
		}
	}
}

/*
===============
G_FreeNode

Free up memory used by a node
===============
*/
static void G_FreeNode( mrNode_t *node )
{
	if ( node->type == NT_CONDITION )
	{
		G_FreeNode( node->u.condition.target );
		BG_Free( node->u.condition.exprString );
	}

	BG_Free( node );
}

/*
===============
G_ShutdownMapRotations

Free up memory used by map rotations
===============
*/
void G_ShutdownMapRotations()
{
	int i, j;

	for ( i = 0; i < mapRotations.numRotations; i++ )
	{
		mapRotation_t *mr = &mapRotations.rotations[ i ];

		for ( j = 0; j < mr->numNodes; j++ )
		{
			mrNode_t *node = mr->nodes[ j ];

			G_FreeNode( node );
		}
	}

	mapRotations = {};
}
