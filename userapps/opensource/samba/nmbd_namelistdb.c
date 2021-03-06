/* 
   Unix SMB/Netbios implementation.
   Version 1.9.
   NBT netbios routines and daemon - version 2
   Copyright (C) Andrew Tridgell 1994-1998
   Copyright (C) Luke Kenneth Casson Leighton 1994-1998
   Copyright (C) Jeremy Allison 1994-1998
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
   
*/

#include "includes.h"

extern int DEBUGLEVEL;

extern pstring scope;
extern char **my_netbios_names;

uint16 samba_nb_type = 0; /* samba's NetBIOS name type */


/****************************************************************************
  Set Samba's NetBIOS name type.
  ****************************************************************************/

void set_samba_nb_type(void)
{
  if (lp_wins_support() || (*lp_wins_server()))
    samba_nb_type = NB_MFLAG; /* samba is a 'hybrid' node type */
  else
    samba_nb_type = NB_BFLAG; /* samba is broadcast-only node type */
}

/****************************************************************************
  Returns True if the netbios name is ^1^2__MSBROWSE__^2^1.

  Note: This name is registered if as a master browser or backup browser
  you are responsible for a workgroup (when you announce a domain by
  broadcasting on your local subnet, you announce it as coming from this
  name: see announce_host()).

  **************************************************************************/

BOOL ms_browser_name(char *name, int type)
{
  return (strequal(name,MSBROWSE) && (type == 0x01));
}

/****************************************************************************
  Add a netbios name into a namelist.
  **************************************************************************/

static void add_name_to_namelist(struct subnet_record *subrec, 
                                 struct name_record *namerec)
{
  struct name_record *namerec2;

  if (!subrec->namelist)
  {
    subrec->namelist = namerec;
    namerec->prev = NULL;
    namerec->next = NULL;
    return;
  }

  for (namerec2 = subrec->namelist; namerec2->next; namerec2 = namerec2->next) 
    ;

  namerec2->next = namerec;
  namerec->next = NULL;
  namerec->prev = namerec2;
  namerec->subnet = subrec;

  subrec->namelist_changed = True;
}

/****************************************************************************
  Remove a name from the namelist.
  **************************************************************************/

void remove_name_from_namelist(struct subnet_record *subrec, 
                               struct name_record *namerec)
{
  if (namerec->next)
    namerec->next->prev = namerec->prev;
  if (namerec->prev)
    namerec->prev->next = namerec->next;

  if(namerec == subrec->namelist)
    subrec->namelist = namerec->next;

  if(namerec->ip != NULL)
    free((char *)namerec->ip);
  free((char *)namerec);

  subrec->namelist_changed = True;
}


/****************************************************************************
  Find a name in a subnet.
  **************************************************************************/

struct name_record *find_name_on_subnet(struct subnet_record *subrec,
                                      struct nmb_name *nmbname, BOOL self_only)
{
  struct name_record *namerec = subrec->namelist;
  struct name_record *name_ret;
  
  for (name_ret = namerec; name_ret; name_ret = name_ret->next)
  {
    if (nmb_name_equal(&name_ret->name, nmbname))
    {
      /* Self names only - these include permanent names. */
      if (self_only && (name_ret->source != SELF_NAME) && 
              (name_ret->source != PERMANENT_NAME) )
      {
        continue;
      }
      DEBUG(9,("find_name_on_subnet: on subnet %s - found name %s source=%d\n", 
                subrec->subnet_name, namestr(nmbname), name_ret->source));
      return name_ret;
    }
  }
  DEBUG(9,("find_name_on_subnet: on subnet %s - name %s NOT FOUND\n", 
            subrec->subnet_name, namestr(nmbname)));
  return NULL;
}

/****************************************************************************
  Find a name over all known broadcast subnets.
**************************************************************************/

struct name_record *find_name_for_remote_broadcast_subnet( struct nmb_name *nmbname, 
                                                           BOOL self_only)
{
  struct subnet_record *subrec;
  struct name_record *namerec = NULL;

  for( subrec = FIRST_SUBNET; subrec; subrec = NEXT_SUBNET_EXCLUDING_UNICAST(subrec))
  {
    if((namerec = find_name_on_subnet(subrec, nmbname, self_only))!= NULL)
      break;
  }

  return namerec;
}    
  
/****************************************************************************
  Update the ttl of an entry in a subnet name list.
  ****************************************************************************/

void update_name_ttl(struct name_record *namerec, int ttl)
{
  time_t time_now = time(NULL);

  if(namerec->death_time != PERMANENT_TTL)
    namerec->death_time = time_now + ttl;

  namerec->refresh_time = time_now + (ttl/2);

  namerec->subnet->namelist_changed = True;
} 

/****************************************************************************
  Add an entry to a subnet name list.
  ****************************************************************************/

struct name_record *add_name_to_subnet(struct subnet_record *subrec,
		char *name, int type, uint16 nb_flags, int ttl, 
                enum name_source source, int num_ips, struct in_addr *iplist)
{
  struct name_record *namerec;
  time_t time_now = time(NULL);

  if((namerec = (struct name_record *)malloc(sizeof(*namerec))) == NULL)
  {
    DEBUG(0,("add_name_to_subnet: malloc fail.\n"));
    return NULL;
  }

  bzero((char *)namerec,sizeof(*namerec));

  namerec->subnet = subrec;

  namerec->num_ips = num_ips;
  namerec->ip = (struct in_addr *)malloc(sizeof(struct in_addr) * namerec->num_ips);
  if (!namerec->ip)
  {
     DEBUG(0,("add_name_to_subnet: malloc fail when creating ip_flgs.\n"));
     free((char *)namerec);
     return NULL;
  }

  bzero((char *)namerec->ip, sizeof(struct in_addr) * namerec->num_ips);

  memcpy(&namerec->ip[0], iplist, num_ips * sizeof(struct in_addr));

  make_nmb_name(&namerec->name,name,type,scope);

  /* Setup the death_time and refresh_time. */
  if(ttl == PERMANENT_TTL)
    namerec->death_time = PERMANENT_TTL;
  else
    namerec->death_time = time_now + ttl;

  namerec->refresh_time = time_now + (ttl/2);

  /* Enter the name as active. */
  namerec->nb_flags = nb_flags | NB_ACTIVE;

  /* If it's our primary name, flag it as so. */
  if(strequal(my_netbios_names[0],name))
    namerec->nb_flags |= NB_PERM;

  namerec->source = source;
  
  add_name_to_namelist(subrec,namerec);

  DEBUG(3,("add_name_to_subnet: Added netbios name %s with first IP %s ttl=%d nb_flags=%2x to subnet %s\n",
	   namestr(&namerec->name),inet_ntoa(*iplist),ttl,(unsigned int)nb_flags,
	   subrec->subnet_name));

  subrec->namelist_changed = True;

  return(namerec);
}

/*******************************************************************
 Utility function automatically called when a name refresh or register 
 succeeds. By definition this is a SELF_NAME (or we wouldn't be registering
 it).
 ******************************************************************/

void standard_success_register(struct subnet_record *subrec, 
                             struct userdata_struct *userdata,
                             struct nmb_name *nmbname, uint16 nb_flags, int ttl,
                             struct in_addr registered_ip)
{
  struct name_record *namerec = find_name_on_subnet(subrec, nmbname, FIND_SELF_NAME);

  if(namerec == NULL)
    add_name_to_subnet(subrec, nmbname->name, nmbname->name_type,
                     nb_flags, ttl, SELF_NAME, 1, &registered_ip);
  else
    update_name_ttl(namerec, ttl);
}

/*******************************************************************
 Utility function automatically called when a name refresh or register 
 fails.
 ******************************************************************/

void standard_fail_register(struct subnet_record *subrec, 
                             struct response_record *rrec, struct nmb_name *nmbname)
{
  struct name_record *namerec = find_name_on_subnet(subrec, nmbname, FIND_SELF_NAME);

  DEBUG(0,("standard_fail_register: Failed to register/refresh name %s on subnet %s\n",
           namestr(nmbname), subrec->subnet_name));

  /* Remove the name from the subnet. */
  if(namerec)
    remove_name_from_namelist(subrec, namerec);
}

/*******************************************************************
 Utility function to remove an IP address from a name record.
 ******************************************************************/

static void remove_nth_ip_in_record( struct name_record *namerec, int ind)
{
  if(ind != namerec->num_ips)
    memmove( (char *)(&namerec->ip[ind]), (char *)(&namerec->ip[ind+1]), 
              ( namerec->num_ips - ind - 1) * sizeof(struct in_addr));

  namerec->num_ips--;
  namerec->subnet->namelist_changed = True;
}

/*******************************************************************
 Utility function to check if an IP address exists in a name record.
 ******************************************************************/

BOOL find_ip_in_name_record(struct name_record *namerec, struct in_addr ip)
{
  int i;

  for(i = 0; i < namerec->num_ips; i++)
    if(ip_equal( namerec->ip[i], ip))
      return True;

  return False;
}

/*******************************************************************
 Utility function to add an IP address to a name record.
 ******************************************************************/

void add_ip_to_name_record(struct name_record *namerec, struct in_addr new_ip)
{
  struct in_addr *new_list;

  /* Don't add one we already have. */
  if(find_ip_in_name_record( namerec, new_ip))
    return;
  
  if((new_list = (struct in_addr *)malloc( (namerec->num_ips + 1)*sizeof(struct in_addr)) )== NULL)
  {
    DEBUG(0,("add_ip_to_name_record: Malloc fail !\n"));
    return;
  }

  memcpy((char *)new_list, (char *)namerec->ip, namerec->num_ips *sizeof(struct in_addr));
  new_list[namerec->num_ips] = new_ip;

  free((char *)namerec->ip);
  namerec->ip = new_list;
  namerec->num_ips += 1;

  namerec->subnet->namelist_changed = True;
}

/*******************************************************************
 Utility function to remove an IP address from a name record.
 ******************************************************************/

void remove_ip_from_name_record( struct name_record *namerec, struct in_addr remove_ip)
{
  /* Try and find the requested ip address - remove it. */
  int i;
  int orig_num = namerec->num_ips;

  for(i = 0; i < orig_num; i++)
    if( ip_equal( remove_ip, namerec->ip[i]) )
    {
      remove_nth_ip_in_record( namerec, i);
      break;
    }
}

/*******************************************************************
 Utility function that release_name callers can plug into as the
 success function when a name release is successful. Used to save
 duplication of success_function code.
 ******************************************************************/

void standard_success_release(struct subnet_record *subrec, 
                             struct userdata_struct *userdata,
                             struct nmb_name *nmbname, struct in_addr released_ip)
{
  struct name_record *namerec = find_name_on_subnet(subrec, nmbname, FIND_ANY_NAME);

  if(namerec == NULL)
  {
    DEBUG(0,("standard_success_release: Name release for name %s IP %s on subnet %s. Name \
was not found on subnet.\n", namestr(nmbname), inet_ntoa(released_ip), subrec->subnet_name));
    return;
  }
  else
  {
    int orig_num = namerec->num_ips;

    remove_ip_from_name_record( namerec, released_ip);

    if(namerec->num_ips == orig_num)
      DEBUG(0,("standard_success_release: Name release for name %s IP %s on subnet %s. This ip \
is not known for this name.\n", namestr(nmbname), inet_ntoa(released_ip), subrec->subnet_name ));
  }

  if (namerec->num_ips == 0)
    remove_name_from_namelist(subrec, namerec);
}

/*******************************************************************
  Expires old names in a subnet namelist.
  ******************************************************************/

void expire_names_on_subnet(struct subnet_record *subrec, time_t t)
{
  struct name_record *namerec;
  struct name_record *next_namerec;

  for (namerec = subrec->namelist; namerec; namerec = next_namerec)
  {
    next_namerec = namerec->next;
    if ((namerec->death_time != PERMANENT_TTL) && (namerec->death_time < t))
    {
      if (namerec->source == SELF_NAME)
      {
        DEBUG(3,("expire_names_on_subnet: Subnet %s not expiring SELF name %s\n", 
             subrec->subnet_name, namestr(&namerec->name)));
        namerec->death_time += 300;
        namerec->subnet->namelist_changed = True;
        continue;
      }
      DEBUG(3,("expire_names_on_subnet: Subnet %s - removing expired name %s\n", 
                 subrec->subnet_name, namestr(&namerec->name)));
  
      remove_name_from_namelist(subrec, namerec);
    }
  }
}

/*******************************************************************
  Expires old names in all subnet namelists.
  ******************************************************************/

void expire_names(time_t t)
{
  struct subnet_record *subrec;

  for (subrec = FIRST_SUBNET; subrec; subrec = NEXT_SUBNET_INCLUDING_UNICAST(subrec))
  {
    expire_names_on_subnet(subrec, t);
  }
}

/****************************************************************************
  Add the magic samba names, useful for finding samba servers.
  These go directly into the name list for a particular subnet,
  without going through the normal registration process.
  When adding them to the unicast subnet, add them as a list of
  all broadcast subnet IP addresses.
**************************************************************************/

void add_samba_names_to_subnet(struct subnet_record *subrec)
{
  struct in_addr *iplist = &subrec->myip;
  int num_ips = 1;

  /* These names are added permanently (ttl of zero) and will NOT be
     refreshed.  */

  if((subrec == unicast_subnet) || (subrec == wins_server_subnet) ||
     (subrec == remote_broadcast_subnet) )
  {
    struct subnet_record *bcast_subrecs;
    int i;
    /* Create an IP list containing all our known subnets. */

    num_ips = iface_count();
    if((iplist = (struct in_addr *)malloc( num_ips * sizeof(struct in_addr) )) == NULL)
    {
      DEBUG(0,("add_samba_names_to_subnet: Malloc fail !\n"));
      return;
    }

    for(bcast_subrecs = FIRST_SUBNET, i = 0; bcast_subrecs; 
                 bcast_subrecs = NEXT_SUBNET_EXCLUDING_UNICAST(bcast_subrecs), i++)
      iplist[i] = bcast_subrecs->myip;

  }

  add_name_to_subnet(subrec,"*",0x0,samba_nb_type, PERMANENT_TTL,
                     PERMANENT_NAME, num_ips, iplist);
  add_name_to_subnet(subrec,"*",0x20,samba_nb_type,PERMANENT_TTL,
                     PERMANENT_NAME, num_ips, iplist);
  add_name_to_subnet(subrec,"__SAMBA__",0x20,samba_nb_type,PERMANENT_TTL,
                   PERMANENT_NAME, num_ips, iplist);
  add_name_to_subnet(subrec,"__SAMBA__",0x00,samba_nb_type,PERMANENT_TTL,
                   PERMANENT_NAME, num_ips, iplist);

  if(iplist != &subrec->myip)
    free((char *)iplist);
}

/****************************************************************************
 Dump the contents of the namelists on all the subnets (including unicast)
 into a file. Initiated by SIGHUP - used to debug the state of the namelists.
**************************************************************************/

static void dump_subnet_namelist( struct subnet_record *subrec, FILE *fp)
{
  struct name_record *namerec;
  char *src_type;
  struct tm *tm;
  int i;

  fprintf(fp, "Subnet %s\n----------------------\n", subrec->subnet_name);
  for (namerec = subrec->namelist; namerec; namerec = namerec->next)
  {
    fprintf(fp,"\tName = %s\t", namestr(&namerec->name));
    switch(namerec->source)
    {
      case LMHOSTS_NAME:
        src_type = "LMHOSTS_NAME";
        break;
      case WINS_PROXY_NAME:
        src_type = "WINS_PROXY_NAME";
        break;
      case REGISTER_NAME:
        src_type = "REGISTER_NAME";
        break;
      case SELF_NAME:
        src_type = "SELF_NAME";
        break;
      case DNS_NAME:
        src_type = "DNS_NAME";
        break;
      case DNSFAIL_NAME:
        src_type = "DNSFAIL_NAME";
        break;
      case PERMANENT_NAME:
        src_type = "PERMANENT_NAME";
        break;
      default:
        src_type = "unknown!";
        break;
    }
    fprintf(fp, "Source = %s\nb_flags = %x\t", src_type, namerec->nb_flags);

    if(namerec->death_time != PERMANENT_TTL)
    {
      tm = LocalTime(&namerec->death_time);
      fprintf(fp, "death_time = %s\t", asctime(tm));
    }
    else
      fprintf(fp, "death_time = PERMANENT\t");

    if(namerec->refresh_time != PERMANENT_TTL)
    {
      tm = LocalTime(&namerec->refresh_time);
      fprintf(fp, "refresh_time = %s\n", asctime(tm));
    }
    else
      fprintf(fp, "refresh_time = PERMANENT\n");

    fprintf(fp, "\t\tnumber of IPS = %d", namerec->num_ips);
    for(i = 0; i < namerec->num_ips; i++)
      fprintf(fp, "\t%s", inet_ntoa(namerec->ip[i]));

    fprintf(fp, "\n\n");
  }
}

/****************************************************************************
 Dump the contents of the namelists on all the subnets (including unicast)
 into a file. Initiated by SIGHUP - used to debug the state of the namelists.
**************************************************************************/

void dump_all_namelists(void)
{
  pstring fname;
  FILE *fp; 
  struct subnet_record *subrec;

  pstrcpy(fname,lp_lockdir());
  trim_string(fname,NULL,"/");
  pstrcat(fname,"/"); 
  pstrcat(fname,"namelist.debug");

  fp = fopen(fname,"w");
     
  if (!fp)
  { 
    DEBUG(0,("dump_all_namelists: Can't open file %s. Error was %s\n",
              fname,strerror(errno)));
    return;
  }
      
  for (subrec = FIRST_SUBNET; subrec ; subrec = NEXT_SUBNET_INCLUDING_UNICAST(subrec))
    dump_subnet_namelist( subrec, fp);

  if(!we_are_a_wins_client())
    dump_subnet_namelist(unicast_subnet, fp);

  if(remote_broadcast_subnet->namelist != NULL)
    dump_subnet_namelist(remote_broadcast_subnet, fp);

  if(wins_server_subnet != NULL)
    dump_subnet_namelist( wins_server_subnet, fp);
  fclose(fp);
}
