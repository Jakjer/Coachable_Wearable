﻿/* FILE : RunList.cs
*PROJECT : Coachable Website
*PROGRAMMER : Stardust Crusaders
*FIRST VERSION : 2020-03-01
*DESCRIPTION : The file contains methods for accessing DB for runs
*/

using System;
using System.Collections.Generic;
using System.Configuration;
using MySql.Data.MySqlClient;
using Newtonsoft.Json;

namespace DemoAPI.Models
{
    /// <summary>
    /// Class for accessing the DB for run information
    /// </summary>
    public class RunList
    {
        /// <summary>
        /// String for connecting to the DB
        /// </summary>
        public string ConnectionString { get; set; }

        /// <summary>
        /// Connection instance to DB
        /// </summary>
        public MySqlConnection Conn { get; set; }

        /// <summary>
        /// Constructor
        /// </summary>
        public RunList()
        {
            ConnectionString = ConfigurationManager.ConnectionStrings["CoachableString"].ConnectionString;
            Conn = new MySqlConnection(ConnectionString);
        }      

        /// <summary>
        /// Method that adds runs to the DB
        /// </summary>
        /// <param name="runs"></param>
        /// <returns>Successful or not</returns>
        public string Insert(List<Run> runs)
        {
            using (Conn)
            {
                // Although this code works, we should add try catches, proper validation, and encryption of data
                try
                {
                    Conn.Open();

                    // Loop through each run sent
                    foreach (Run newRun in runs)
                    {
                        // Create and execute query for finding the users id using the device name
                        const string findUserQuery = @"SELECT user_id FROM Devices WHERE Device_Name = @name";
                        var userCommand = new MySqlCommand(findUserQuery, Conn);
                        userCommand.Parameters.AddWithValue("@name", newRun.DeviceName);
                        var userID = userCommand.ExecuteScalar();
                        newRun.UserID = int.Parse(userID.ToString());

                        // Create and execute query for finding the users team based on their id
                        const string findTeamQuery = @"SELECT team_id FROM UserTeams WHERE user_id = @userID";
                        var teamCommand = new MySqlCommand(findTeamQuery, Conn);
                        teamCommand.Parameters.AddWithValue("@userID", newRun.UserID);
                        var teamID = userCommand.ExecuteScalar();

                        // Create and execute query for finding an event that matches the date provided by device
                        const string findEventQuery = @"SELECT id FROM Events WHERE team_id = @teamID AND Event_Date = @date";
                        var eventCommand = new MySqlCommand(findEventQuery, Conn);
                        eventCommand.Parameters.AddWithValue("@teamID", teamID);
                        eventCommand.Parameters.AddWithValue("@userID", newRun.Date);
                        var eventID = userCommand.ExecuteScalar();
                        newRun.EventID = int.Parse(eventID.ToString());

                        //Create and execute query for inserting the run into the DB
                        const string insertQuery = @"INSERT INTO Runs(user_id, event_id, duration, date, start_time, end_time, start_altitude, end_altitude, avg_speed, distance, other_data) VALUES
                                                 (@userID, @eventid, @duration, @date, @startTime, @endTime, @startAltitude, @endAltitude, @AvgSpeed, @distance, @data)";
                        var insertCommand = new MySqlCommand(insertQuery, Conn);
                        insertCommand.Parameters.AddWithValue("@userID", newRun.UserID);
                        insertCommand.Parameters.AddWithValue("@eventid", newRun.EventID);
                        insertCommand.Parameters.AddWithValue("@duration", newRun.Duration);
                        insertCommand.Parameters.AddWithValue("@date", newRun.Date);
                        insertCommand.Parameters.AddWithValue("@startTime", newRun.StartTime);
                        insertCommand.Parameters.AddWithValue("@endTime", newRun.EndTime);
                        insertCommand.Parameters.AddWithValue("@startAltitude", newRun.StartAltitude);
                        insertCommand.Parameters.AddWithValue("@endAltitude", newRun.EndAltitude);
                        insertCommand.Parameters.AddWithValue("@AvgSpeed", newRun.AvgSpeed);
                        insertCommand.Parameters.AddWithValue("@distance", newRun.Distance);

                        //Serialize the data field and fill in
                        string dataString = JsonConvert.SerializeObject(newRun.Data);
                        insertCommand.Parameters.AddWithValue("@data", dataString);
                        insertCommand.ExecuteNonQuery();
                    }

                    return "Runs have been added!";
                }
                catch(Exception e)
                {
                    return "Error: " + e.Message;
                }          
            }            
        }
    }
}