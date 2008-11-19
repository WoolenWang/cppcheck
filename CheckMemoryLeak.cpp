/*
 * c++check - c/c++ syntax checking
 * Copyright (C) 2007 Daniel Marjamäki
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/
 */


#include "CheckMemoryLeak.h"



#include "CommonCheck.h"

#include <stdlib.h> // free

#include <algorithm>

#include <sstream>

#ifdef __BORLANDC__
#include <mem.h>     // <- memset
#else
#include <string.h>
#endif

#ifndef _MSC_VER
#define _strdup(str) strdup(str)
#endif

//---------------------------------------------------------------------------

CheckMemoryLeakClass::CheckMemoryLeakClass( Tokenizer *tokenizer, const Settings &settings )
{
    _tokenizer = tokenizer;
    _settings = settings;
}

CheckMemoryLeakClass::~CheckMemoryLeakClass()
{

}

bool CheckMemoryLeakClass::isclass( const std::string &typestr )
{
    if ( typestr == "char" ||
         typestr == "short" ||
         typestr == "int" ||
         typestr == "long" ||
         typestr == "float" ||
         typestr == "double" )
        return false;

    std::ostringstream pattern;
    pattern << "struct " << typestr;
    if ( findmatch( _tokenizer->tokens(), pattern.str().c_str() ) )
        return false;

    return true;
}
//---------------------------------------------------------------------------

AllocType CheckMemoryLeakClass::GetAllocationType( const TOKEN *tok2 )
{
    // What we may have...
    //     * var = (char *)malloc(10);
    //     * var = new char[10];
    //     * var = strdup("hello");
    if ( tok2 && tok2->str[0] == '(' )
    {
        while ( tok2 && tok2->str[0] != ')' )
            tok2 = tok2->next;
        tok2 = tok2 ? tok2->next : NULL;
    }
    if ( ! tok2 )
        return No;

    // Does tok2 point on "malloc", "strdup" or "kmalloc"..
    const char *mallocfunc[] = {"malloc",
                                "calloc",
                                "realloc",
                                "strdup",
                                "kmalloc",
                                "kzalloc",
                                0};
    for ( unsigned int i = 0; mallocfunc[i]; i++ )
    {
        if ( strcmp(mallocfunc[i], tok2->str) == 0 )
            return Malloc;
    }

    // Does tok2 point on "malloc", "strdup" or "kmalloc"..
    const char *gmallocfunc[] = {"g_new",
                                 "g_new0",
                                 "g_renew",
                                 "g_try_new",
                                 "g_try_new0",
                                 "g_try_renew",
                                 "g_malloc",
                                 "g_malloc0",
                                 "g_realloc",
                                 "g_try_malloc",
                                 "g_try_malloc0",
                                 "g_try_realloc",
                                 "g_strdup",
                                 "g_strndup",
                                 0};
    for ( unsigned int i = 0; gmallocfunc[i]; i++ )
    {
        if ( strcmp(gmallocfunc[i], tok2->str) == 0 )
            return gMalloc;
    }

    if ( Match( tok2, "new %type% [;(]" ) )
        return New;

    if ( Match( tok2, "new %type% [" ) )
        return NewA;

    // Userdefined allocation function..
    std::list<AllocFunc>::const_iterator it = listallocfunc.begin();
    while ( it != listallocfunc.end() )
    {
        if ( strcmp(tok2->str, it->funcname) == 0 )
            return it->alloctype;
        ++it;
    }

    return No;
}

AllocType CheckMemoryLeakClass::GetDeallocationType( const TOKEN *tok, const char *varnames[] )
{
    // Redundant condition..
    if ( Match(tok, "if ( %var1% )", varnames) )
    {
        tok = Tokenizer::gettok( tok, 4 );
        if ( Match(tok,"{") )
            tok = tok->next;
    }

    if ( Match(tok, "delete %var1% ;", varnames) )
        return New;

    if ( Match(tok, "delete [ ] %var1% ;", varnames) )
        return NewA;

    if ( Match(tok, "free ( %var1% ) ;", varnames) ||
         Match(tok, "kfree ( %var1% ) ;", varnames) )
    {
        return Malloc;
    }

    if ( Match(tok, "g_free ( %var1% ) ;", varnames) )
        return gMalloc;

    return No;
}
//--------------------------------------------------------------------------

const char * CheckMemoryLeakClass::call_func( const TOKEN *tok, std::list<const TOKEN *> callstack, const char *varnames[], AllocType &alloctype, AllocType &dealloctype )
{
    if (Match(tok,"if") || Match(tok,"for") || Match(tok,"while"))
        return 0;

    if (GetAllocationType(tok)!=No || GetDeallocationType(tok,varnames)!=No)
        return 0;

    if ( callstack.size() > 2 )
        return 0;

    const char *funcname = tok->str;
    for ( std::list<const TOKEN *>::const_iterator it = callstack.begin(); it != callstack.end(); ++it )
    {
        if ( std::string(funcname) == (*it)->str )
            return 0;
    }
    callstack.push_back(tok);

    int par = 1;
    int parlevel = 0;
    for ( ; tok; tok = tok->next )
    {
        if ( Match(tok, "(") )
            ++parlevel;
        else if ( Match(tok, ")") )
        {
            --parlevel;
            if ( parlevel < 1 )
                return NULL;
        }

        if ( parlevel == 1 )
        {
            if ( Match(tok, ",") )
                ++par;
            if ( Match(tok, "[,()] %var1% [,()]", varnames) )
            {
                const TOKEN *ftok = _tokenizer->GetFunctionTokenByName(funcname);
                const char *parname = GetParameterName( ftok, par );
                if ( ! parname )
                    return "use";
                // Check if the function deallocates the variable..
                while ( ftok && ! Match(ftok,"{") )
                    ftok = ftok->next;
                TOKEN *func = getcode( Tokenizer::gettok(ftok,1), callstack, parname, alloctype, dealloctype );
                simplifycode( func );
                const char *ret = 0;
                if (findmatch(func, "goto"))
                    ret = 0;    // TODO : "goto" isn't handled well
                else if (findmatch(func, "use"))
                    ret = "use";
                else if (findmatch(func, "dealloc"))
                    ret = "dealloc";
                deleteTokens(func);
                return ret;
            }
        }
    }
    return NULL;
}

//--------------------------------------------------------------------------

void CheckMemoryLeakClass::MismatchError( const TOKEN *Tok1, const std::list<const TOKEN *> &callstack, const char varname[] )
{
    std::ostringstream errmsg;
    for ( std::list<const TOKEN *>::const_iterator tok = callstack.begin(); tok != callstack.end(); ++tok )
        errmsg << FileLine(*tok, _tokenizer) << " -> ";
    errmsg << FileLine(Tok1, _tokenizer) << ": Mismatching allocation and deallocation: " << varname;
    ReportErr( errmsg.str() );
}
//---------------------------------------------------------------------------

void CheckMemoryLeakClass::MemoryLeak( const TOKEN *tok, const char varname[] )
{
    std::ostringstream errmsg;
    errmsg << FileLine(tok, _tokenizer) << ": Memory leak: " << varname;
    ReportErr( errmsg.str() );
}
//---------------------------------------------------------------------------

void CheckMemoryLeakClass::instoken(TOKEN *tok, const char str[])
{
    TOKEN *newtok = new TOKEN;
    newtok->setstr(str);
    newtok->next = tok->next;
    tok->next = newtok;
}
//---------------------------------------------------------------------------

bool CheckMemoryLeakClass::notvar(const TOKEN *tok, const char *varnames[])
{
    return bool( Match(tok, "! %var1% [;)&|]", varnames) ||
                 Match(tok, "! ( %var1% )", varnames) ||
                 Match(tok, "unlikely ( ! %var1% )", varnames) ||
                 Match(tok, "unlikely ( %var1% == 0 )", varnames) ||
                 Match(tok, "0 == %var1% [;)&|]", varnames) ||
                 Match(tok, "%var1% == 0", varnames) );
}

/**
 * Extract a new tokens list that is easier to parse than the "tokens"
 * tok - start parse token
 * varname - name of variable
 */

TOKEN *CheckMemoryLeakClass::getcode(const TOKEN *tok, std::list<const TOKEN *> callstack, const char varname[], AllocType &alloctype, AllocType &dealloctype)
{
    const char *varnames[2];
    varnames[0] = varname;
    varnames[1] = 0;

    TOKEN *rethead = 0, *rettail = 0;
    #define addtoken(_str)                  \
    {                                       \
        TOKEN *newtok = new TOKEN;          \
        newtok->setstr(_str);               \
        newtok->linenr = tok->linenr;       \
        newtok->FileIndex = tok->FileIndex; \
        newtok->next = 0;                   \
        if (rettail)                        \
            rettail->next = newtok;         \
        else                                \
            rethead = newtok;               \
        rettail=newtok;                     \
    }

    bool isloop = false;

    int indentlevel = 0;
    int parlevel = 0;
    for ( ; tok; tok = tok->next )
    {
        if ( tok->str[0] == '{' )
        {
            addtoken( "{" );
            indentlevel++;
        }
        else if ( tok->str[0] == '}' )
        {
            addtoken( "}" );
            if ( indentlevel <= 0 )
                break;
            indentlevel--;
        }

        if ( tok->str[0] == '(' )
            parlevel++;
        else if ( tok->str[0] == ')' )
            parlevel--;
        isloop &= ( parlevel > 0 );

        if ( parlevel == 0 && tok->str[0]==';')
            addtoken(";");

        if (Match(tok, "[(;{}] %var1% =", varnames))
        {
            AllocType alloc = GetAllocationType(Tokenizer::gettok(tok,3));

            // If "--all" hasn't been given, don't check classes..
            if ( alloc == New && ! _settings._showAll )
            {
                if ( Match(Tokenizer::gettok(tok,3), "new %type% [(;]") )
                {
                    if ( isclass( Tokenizer::getstr(tok, 4) ) )
                        alloc = No;
                }
            }

            if ( alloc != No )
            {
                addtoken("alloc");
                if (alloctype!=No && alloctype!=alloc)
                    MismatchError(tok, callstack, varname);
                if (dealloctype!=No && dealloctype!=alloc)
                    MismatchError(tok, callstack, varname);
                alloctype = alloc;
            }
        }

        AllocType dealloc = GetDeallocationType(tok, varnames);
        if ( dealloc != No )
        {
            addtoken("dealloc");
            if (alloctype!=No && alloctype!=dealloc)
                MismatchError(tok, callstack, varname);
            if (dealloctype!=No && dealloctype!=dealloc)
                MismatchError(tok, callstack, varname);
            dealloctype = dealloc;
        }

        // if else switch
        if ( Match(tok, "if ( %var1% )", varnames) ||
             Match(tok, "if ( %var1% != 0 )", varnames) ||
             Match(tok, "if ( 0 != %var1% )", varnames)  )
        {
            addtoken("if(var)");
            tok = Tokenizer::gettok(tok, 3);   // Make sure the "use" will not be added
        }
        else if ( Match(tok, "if (") && notvar(Tokenizer::gettok(tok,2), varnames) )
        {
            addtoken("if(!var)");
        }
        else if ( Match(tok, "if")     ||
                  Match(tok, "else")   ||
                  Match(tok, "switch") )
        {
            addtoken(tok->str);
        }

        if ( Match(tok, "case") )
        {
            addtoken("case");
            addtoken(";");
        }

        if ( Match(tok, "default") )
        {
            addtoken("case");
            addtoken(";");
        }

        // Loops..
        if (Match(tok, "for") || Match(tok, "while") )
        {
            addtoken("loop");
            isloop = true;
        }
        if ( Match(tok, "do") )
        {
            addtoken("do");
        }
        if ( isloop && notvar(tok,varnames) )
            addtoken("!var");

        // continue / break..
        if ( Match(tok, "continue") )
            addtoken("continue");
        if ( Match(tok, "break") )
            addtoken("break");

        // goto..
        if ( Match(tok, "goto") )
        {
            addtoken("goto");
        }

        // Return..
        if ( Match(tok, "return") )
        {
            addtoken("return");
            if ( Match(tok, "return %var1%", varnames) ||
                 Match(tok, "return & %var1%", varnames) )
                addtoken("use");
        }

        // throw..
        if ( Match(tok, "throw") )
            addtoken("throw");

        // Assignment..
        if ( Match(tok,"[)=] %var1% [;)]", varnames) )
            addtoken("use");

        // Investigate function calls..
        if ( Match(tok, "%var% (") )
        {
            const char *str = call_func(tok, callstack, varnames, alloctype, dealloctype);
            if ( str )
                addtoken( str );
        }

        // Linux lists..
        if ( Match( tok, "[=(,] & %var1% [.[]", varnames ) )
        {
            // todo: better checking
            addtoken("use");
        }
    }

    return rethead;
}

void CheckMemoryLeakClass::erase(TOKEN *begin, const TOKEN *end)
{
    if ( ! begin )
        return;

    while ( begin->next && begin->next != end )
    {
        TOKEN *next = begin->next;
        begin->next = begin->next->next;
        delete next;
    }
}



/**
 * Simplify code
 * \param tok first token
 */
void CheckMemoryLeakClass::simplifycode(TOKEN *tok)
{
    // Remove "do"...
    // do { x } while (y);
    // =>
    // { x } while(y) { x }"
    for ( TOKEN *tok2 = tok; tok2; tok2 = tok2->next )
    {
        if ( ! Match(tok2->next, "do") )
            continue;

        // Remove the next token "do"
        erase( tok2, Tokenizer::gettok(tok2, 2) );
        tok2 = tok2->next;

        // Find the end of the "do" block..
        TOKEN *tok2_;
        int indentlevel = 0;
        for ( tok2_ = tok2; tok2_ && indentlevel>=0; tok2_ = tok2_->next )
        {
            if ( Match(tok2_, "{") )
                ++indentlevel;

            else if ( Match(tok2_, "}") )
                --indentlevel;

            else if ( indentlevel == 0 && Match(tok2_->next, ";") )
                break;
        }

        // End not found?
        if ( ! tok2_ )
            continue;

        // Copy code..
        indentlevel = 0;
        do
        {
            if ( Match( tok2, "{" ) )
                ++indentlevel;
            else if ( Match(tok2, "}") )
                --indentlevel;

            // Copy token..
            instoken( tok2_, tok2->str );

            // Next token..
            tok2 = tok2->next;
            tok2_ = tok2_->next;
        }
        while ( tok2 && indentlevel > 0 );
    }


    // reduce the code..
    bool done = false;
    while ( ! done )
    {
        done = true;

        for (TOKEN *tok2 = tok; tok2; tok2 = tok2 ? tok2->next : NULL )
        {
            // Delete extra ";"
            while (Match(tok2,"[;{}] ;"))
            {
                erase(tok2, Tokenizer::gettok(tok2,2));
                done = false;
            }

            // Replace "{ }" with ";"
            if ( Match(tok2->next, "{ }") )
            {
                tok2->next->setstr(";");
                erase(tok2->next, Tokenizer::gettok(tok2,3));
                done = false;
            }

            // Delete braces around a single instruction..
            if ( Match(tok2->next, "{ %var% ; }") )
            {
                erase( tok2, Tokenizer::gettok(tok2,2) );
                erase( tok2->next->next, Tokenizer::gettok(tok2,4) );
                done = false;
            }
            if ( Match(tok2->next, "{ return use ; }") )
            {
                erase( tok2, Tokenizer::gettok(tok2,2) );
                erase( tok2->next->next->next, Tokenizer::gettok(tok2,5) );
                done = false;
            }

            // Delete empty if that is not followed by an else
            if ( Match(tok2,"[;{}] if ;") ||
                 Match(tok2,"[;{}] if(var) ;") ||
                 Match(tok2,"[;{}] if(!var) ;") )
            {
                if ( ! Match(Tokenizer::gettok(tok2,3), "else") )
                {
                    erase(tok2, Tokenizer::gettok(tok2, 3));
                    done = false;
                    continue;
                }
            }

            // Delete "if dealloc ;" and "if use ;" that is not followed by an else..
            // This may cause false positives
            if (_settings._showAll &&
                (Match(tok2, "[;{}] if dealloc ;") || Match(tok2, "[;{}] if use ;")) &&
                !Match(Tokenizer::gettok(tok2,4), "else"))
            {
                erase(tok2->next, Tokenizer::gettok(tok2,3));
                done = false;
            }

            // Delete if block: "alloc; if return use ;"
            if (Match(tok2,"alloc ; if return use ;") && !Match(Tokenizer::gettok(tok2,6),"else"))
            {
                erase(tok2, Tokenizer::gettok(tok2,5));
                done = false;
            }

            // "[;{}] if alloc ; else return ;" => "[;{}] alloc ;"
            if (Match(tok2,"[;{}] if alloc ; else return ;"))
            {
                erase(tok2, Tokenizer::gettok(tok2,2));        // Remove "if"
                erase(tok2->next, Tokenizer::gettok(tok2,5));  // Remove "; else return"
                done = false;
            }

            // Replace "dealloc use ;" with "dealloc ;"
            if ( Match(tok2, "dealloc use ;") )
            {
                erase(tok2, Tokenizer::gettok(tok2,2));
                done = false;
            }

            // Reducing if..
            if (Match(tok2,"if dealloc ; else") || Match(tok2,"if use ; else"))
            {
                erase(tok2, Tokenizer::gettok(tok2, 2));
                done = false;
            }
            if (Match(tok2,"[;{}] if { dealloc ; return ; }") && !Match(Tokenizer::gettok(tok2,8),"else"))
            {
                erase(tok2,Tokenizer::gettok(tok2,8));
                done = false;
            }

            // Replace "loop ;" with ";"
            if ( Match(tok2->next, "loop ;") )
            {
                erase(tok2, Tokenizer::gettok(tok2,2));
                done = false;
            }

            // Replace "loop !var ;" with ";"
            if ( Match(tok2->next, "loop !var ;") )
            {
                erase(tok2, Tokenizer::gettok(tok2,4));
                done = false;
            }

            // Replace "loop !var alloc ;" with " alloc ;"
            if ( Match(tok2->next, "loop !var alloc ;") )
            {
                erase(tok2, Tokenizer::gettok(tok2,3));
                done = false;
            }

            // Delete if block in "alloc ; if(!var) return ;"
            if ( Match(tok2, "alloc ; if(!var) return ;") )
            {
                erase(tok2, Tokenizer::gettok(tok2,4));
                done = false;
            }

            // Delete second use in "use ; use ;"
            while (Match(tok2, "[;{}] use ; use ;"))
            {
                erase(tok2, Tokenizer::gettok(tok2,3));
                done = false;
            }

            // Delete second case in "case ; case ;"
            while (Match(tok2, "case ; case ;"))
            {
                erase(tok2, Tokenizer::gettok(tok2,3));
                done = false;
            }

            // Replace switch with if (if not complicated)
            if (Match(tok2, "switch {"))
            {
                // Right now, I just handle if there are a few case and perhaps a default.
                bool valid = false;
                bool incase = false;
                for ( const TOKEN * _tok = Tokenizer::gettok(tok2,2); _tok; _tok = _tok->next )
                {
                    if ( _tok->str[0] == '{' )
                        break;

                    else if ( _tok->str[0] == '}' )
                    {
                        valid = true;
                        break;
                    }

                    else if (strncmp(_tok->str,"if",2)==0)
                        break;

                    else if (strcmp(_tok->str,"switch")==0)
                        break;

                    else if (strcmp(_tok->str,"loop")==0)
                        break;

                    else if (incase && Match(_tok,"case"))
                        break;

                    incase |= Match(_tok,"case");
                    incase &= !Match(_tok,"break");
                }

                if ( !incase && valid )
                {
                    done = false;
                    tok2->setstr(";");
                    erase( tok2, Tokenizer::gettok(tok2, 2) );
                    tok2 = tok2->next;
                    bool first = true;
                    while (Match(tok2,"case") || Match(tok2,"default"))
                    {
                        bool def = Match(tok2, "default");
                        tok2->setstr(first ? "if" : "}");
                        if ( first )
                        {
                            first = false;
                            instoken( tok2, "{" );
                        }
                        else
                        {
                            // Insert "else [if] {
                            instoken( tok2, "{" );
                            if ( ! def )
                                instoken( tok2, "if" );
                            instoken( tok2, "else" );
                        }
                        while ( tok2 && tok2->str[0] != '}' && ! Match(tok2,"break ;") )
                            tok2 = tok2->next;
                        if (Match(tok2,"break ;"))
                        {
                            tok2->setstr(";");
                            tok2 = tok2->next->next;
                        }
                    }
                }
            }

            if ( Match(tok2, "throw") )
            {
                tok2->setstr( "return" );
                done = false;
            }
        }
    }
}





// Simpler but less powerful than "CheckMemoryLeak_CheckScope_All"
void CheckMemoryLeakClass::CheckMemoryLeak_CheckScope( const TOKEN *Tok1, const char varname[] )
{
    std::list<const TOKEN *> callstack;

    AllocType alloctype = No;
    AllocType dealloctype = No;

    TOKEN *tok = getcode( Tok1, callstack, varname, alloctype, dealloctype );

    // If the variable is not allocated at all => no memory leak
    if (findmatch(tok, "alloc") == 0)
    {
        deleteTokens(tok);
        return;
    }

    // TODO : handle "goto"
    if (findmatch(tok, "goto"))
    {
        deleteTokens(tok);
        return;
    }

    simplifycode( tok );

    if ( findmatch(tok, "loop alloc ;") )
    {
        MemoryLeak(findmatch(tok, "loop alloc ;"), varname);
    }

    else if ( _settings._showAll && findmatch(tok, "alloc ; if continue ;") )
    {
        MemoryLeak(Tokenizer::gettok(findmatch(tok, "alloc ; if continue ;"), 3), varname);
    }

    else if ( _settings._showAll && findmatch(tok, "alloc ; if break ;") )
    {
        MemoryLeak(Tokenizer::gettok(findmatch(tok, "alloc ; if break ;"), 3), varname);
    }

    else if ( _settings._showAll && findmatch(tok, "alloc ; if return ;") )
    {
        MemoryLeak(Tokenizer::gettok(findmatch(tok, "alloc ; if return ;"), 3), varname);
    }

    else if ( findmatch(tok, "alloc ; return ;") )
    {
        MemoryLeak(Tokenizer::gettok(findmatch(tok,"alloc ; return ;"),2), varname);
    }

    else if ( findmatch(tok, "alloc ; alloc") )
    {
        MemoryLeak(Tokenizer::gettok(findmatch(tok,"alloc ; alloc"),2), varname);
    }

    else if ( ! findmatch(tok,"dealloc") &&
              ! findmatch(tok,"use") &&
              ! findmatch(tok,"return use ;") )
    {
        const TOKEN *last = tok;
        while (last->next)
            last = last->next;
        MemoryLeak(last, varname);
    }

    deleteTokens(tok);
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// Checks for memory leaks inside function..
//---------------------------------------------------------------------------

void CheckMemoryLeakClass::CheckMemoryLeak_InFunction()
{
    bool infunc = false;
    int indentlevel = 0;
    for (const TOKEN *tok = _tokenizer->tokens(); tok; tok = tok->next)
    {
        if (tok->str[0]=='{')
            indentlevel++;

        else if (tok->str[0]=='}')
            indentlevel--;


        // In function..
        if ( indentlevel == 0 )
        {
            if ( Match(tok, ") {") )
                infunc = true;

            if ( Match(tok, "[;}]") )
                infunc = false;
        }

        // Declare a local variable => Check
        if (indentlevel>0 && infunc)
        {
            if ( Match(tok, "[{};] %type% * %var% [;=]") )
                CheckMemoryLeak_CheckScope( tok->next, Tokenizer::getstr(tok, 3) );

            else if ( Match(tok, "[{};] %type% %type% * %var% [;=]") )
                CheckMemoryLeak_CheckScope( tok->next, Tokenizer::getstr(tok, 4) );
        }
    }
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// Checks for memory leaks in classes..
//---------------------------------------------------------------------------



void CheckMemoryLeakClass::CheckMemoryLeak_ClassMembers()
{
    int indentlevel = 0;
    for ( const TOKEN *tok = _tokenizer->tokens(); tok; tok = tok->next )
    {
        if ( tok->str[0] == '{' )
            indentlevel++;

        else if ( tok->str[0] == '}' )
            indentlevel--;

        else if ( indentlevel == 0 && Match(tok, "class %var% [{:]") )
        {
            std::vector<const char *> classname;
            classname.push_back( Tokenizer::getstr(tok, 1) );
            CheckMemoryLeak_ClassMembers_ParseClass( tok, classname );
        }
    }
}


void CheckMemoryLeakClass::CheckMemoryLeak_ClassMembers_ParseClass( const TOKEN *tok1, std::vector<const char *> &classname )
{
    // Go into class.
    while ( tok1 && tok1->str[0] != '{' )
        tok1 = tok1->next;
    if ( tok1 )
        tok1 = tok1->next;

    int indentlevel = 0;
    for ( const TOKEN *tok = tok1; tok; tok = tok->next )
    {
        if ( tok->str[0] == '{' )
            indentlevel++;

        else if ( tok->str[0] == '}' )
        {
            indentlevel--;
            if ( indentlevel < 0 )
                return;
        }

        // Only parse this particular class.. not subclasses
        if ( indentlevel > 0 )
            continue;

        // Declaring subclass.. recursive checking
        if ( Match(tok, "class %var% [{:]") )
        {
            classname.push_back( Tokenizer::getstr(tok, 1) );
            CheckMemoryLeak_ClassMembers_ParseClass( tok, classname );
            classname.pop_back();
        }

        // Declaring member variable.. check allocations and deallocations
        if ( Match(tok->next, "%type% * %var% ;") )
        {
            if ( IsName(tok->str) || strchr(";}", tok->str[0]) )
            {
                if (_settings._showAll || !isclass(Tokenizer::getstr(tok,1)))
                    CheckMemoryLeak_ClassMembers_Variable( classname, Tokenizer::getstr(tok, 3) );
            }
        }
    }
}

void CheckMemoryLeakClass::CheckMemoryLeak_ClassMembers_Variable( const std::vector<const char *> &classname, const char varname[] )
{
    // Function pattern.. Check if member function
	std::ostringstream fpattern;
    for ( unsigned int i = 0; i < classname.size(); i++ )
    {
		fpattern << classname[i] << " :: ";
    }
    fpattern << "%var% (";

    // Destructor pattern.. Check if class destructor..
	std::ostringstream destructor;
    for ( unsigned int i = 0; i < classname.size(); i++ )
    {
		destructor << classname[i] << " :: ";
    }
    destructor << " ~" << classname.back() << " (";

    // Pattern used in member function. "Var = ..."
    std::ostringstream varname_eq;
    varname_eq << varname << " =";

    // Full variable name..
    std::ostringstream FullVariableName;
    for ( unsigned int i = 0; i < classname.size(); i++ )
        FullVariableName << classname[i] << "::";
    FullVariableName << varname;

    // Check if member variable has been allocated and deallocated..
    AllocType Alloc = No;
    AllocType Dealloc = No;

    // Loop through all tokens. Inspect member functions
    bool memberfunction = false;
    int indentlevel = 0;
    for ( const TOKEN *tok = _tokenizer->tokens(); tok; tok = tok->next )
    {
        if ( tok->str[0] == '{' )
            indentlevel++;

        else if ( tok->str[0] == '}' )
            indentlevel--;

        // Set the 'memberfunction' variable..
        if ( indentlevel == 0 )
        {
            if ( strchr(";}", tok->str[0]) )
                memberfunction = false;
            else if ( Match( tok, fpattern.str().c_str() ) || Match( tok, destructor.str().c_str() ) )
                memberfunction = true;
        }

        // Parse member function..
        if ( indentlevel > 0 && memberfunction )
        {
            // Allocate..
            if ( Match( tok, varname_eq.str().c_str() ) )
            {
                AllocType alloc = GetAllocationType( Tokenizer::gettok( tok, 2 ) );
                if ( alloc != No )
                {
                    std::list<const TOKEN *> callstack;
                    if ( Dealloc != No && Dealloc != alloc )
                        MismatchError( tok, callstack, FullVariableName.str().c_str() );
                    if ( Alloc != No && Alloc != alloc )
                        MismatchError( tok, callstack, FullVariableName.str().c_str() );
                    Alloc = alloc;
                }
            }

            // Deallocate..
            const char *varnames[2] = { "var", 0 };
            varnames[0] = varname;
            AllocType dealloc = GetDeallocationType( tok, varnames );
            if ( dealloc != No )
            {
                std::list<const TOKEN *> callstack;
                if ( Dealloc != No && Dealloc != dealloc )
                    MismatchError( tok, callstack, FullVariableName.str().c_str() );
                if ( Alloc != No && Alloc != dealloc )
                    MismatchError( tok, callstack, FullVariableName.str().c_str() );
                Dealloc = dealloc;
            }
        }
    }

    if ( Alloc != No && Dealloc == No )
    {
        MemoryLeak( _tokenizer->tokens(), FullVariableName.str().c_str() );
    }
}




//---------------------------------------------------------------------------
// Checks for memory leaks..
//---------------------------------------------------------------------------

void CheckMemoryLeakClass::CheckMemoryLeak()
{
    listallocfunc.clear();

    // Check for memory leaks inside functions..
    CheckMemoryLeak_InFunction();

    // Check that all class members are deallocated..
    CheckMemoryLeak_ClassMembers();
}
//---------------------------------------------------------------------------





