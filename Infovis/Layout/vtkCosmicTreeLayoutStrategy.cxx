#include "vtkCosmicTreeLayoutStrategy.h"

#include "vtkObjectFactory.h"
#include "vtkDataSetAttributes.h"
#include "vtkDoubleArray.h"
#include "vtkIdTypeArray.h"
#include "vtkMath.h"
#include "vtkPoints.h"
#include "vtkTree.h"

#include "vtksys/ios/sstream"

#include <vector>
#include <algorithm>

#include <math.h>

#ifdef VTK_USE_BOOST
# include "vtkBoostBreadthFirstSearchTree.h"
#endif

// Define to print debug showing convergence (or lack thereof) of loop to find enclosing radius, Re
#undef VTK_COSMIC_DBG

vtkStandardNewMacro(vtkCosmicTreeLayoutStrategy);

/// Represent a circle to be placed
class vtkCosmicTreeEntry
{
public:
  vtkCosmicTreeEntry( vtkIdType id, vtkIdType index, double radius )
    {
    this->Radius = fabs( radius );
    this->Index = index;
    this->Id = id;
    this->Alpha = 0.;
    for ( int i = 0; i < 3; ++ i )
      this->Center[i] = 0.;
    }
  void ComputeCenterFromAlpha( double Re )
    {
    double R = Re - this->Radius;
    this->Center[0] = R * cos( this->Alpha );
    this->Center[1] = R * sin( this->Alpha );
    }
  double PlaceCounterClockwise( const vtkCosmicTreeEntry& neighbor, double Re )
    {
    double ri = neighbor.Radius;
    double rj = this->Radius;
    double Ri = Re - ri;
    double Rj = Re - rj;
    double rij = ri + rj;
    double dRe = Re - rij;
    if ( dRe < 0 )
      {
      // Circles will not fit in another of radius Re.
      // Return how much to increment Re so that they will.
      this->Alpha = neighbor.Alpha + vtkMath::Pi();
      this->ComputeCenterFromAlpha( Re );
      return -dRe;
      }
    // OK, expect a good answer from acos().
    this->Alpha = neighbor.Alpha + acos( ( rij * rij - ( Ri * Ri + Rj * Rj ) ) / ( -2. * Ri * Rj ) );
    this->ComputeCenterFromAlpha( Re );
    return 0.;
    }
  double Defect( const vtkCosmicTreeEntry& other ) const
    {
    // Assumes Center is valid
    double d = 0.;
    for ( int i = 0; i < 2; ++ i )
      {
      double s = this->Center[i] - other.Center[i];
      d += s * s;
      }
    // tangent circles should return 0.0. Overlapping circles return > 0. Values <= 0.0 OK.
    return this->Radius + other.Radius - sqrt( d );
    }
  double Defect( const vtkCosmicTreeEntry& neighbor, double Re )
    {
    double ri = neighbor.Radius;
    double rj = this->Radius;
    double rij = ri + rj;
    return rij - Re;
    }
  bool operator < ( const vtkCosmicTreeEntry& other ) const
    {
    // Note reversed checks for Radius. we want sorted in descending order...
    if ( this->Radius > other.Radius )
      return true;
    else if ( this->Radius < other.Radius )
      return false;

    if ( this->Index < other.Index )
      return true;
    else if ( this->Index > other.Index )
      return false;

    if ( this->Id < other.Id )
      return true;
    return false;
    }
  double Radius;
  double Alpha;
  vtkIdType Index;
  vtkIdType Id;
  double Center[3];
};

/**\brief Lay out a single level quickly.
  *
  * This computes coordinates for the center of each node given a set of unsorted input radii.
  * The nodes are returned sorted from highest radius to lowest and with the node center coordinates set.
  * The enclosing circle has its center at the origin and its radius is returned in \a Re.
  *
  * This version does not allow the largest input circle to touch the center of the enclosing circle
  * whose radius, \a Re, we are computing.
  * Also, the placements generated by this method will not leave circles tangent but will guarantee
  * that each circle "owns" some positive angular slice of the enclosing circle's area (meaning that
  * there is a straight, unobstructed path to the center of the enclosing circle from the center of
  * each input circle).
  *
  * @param[in] N The number of nodes (technically not needed since circles.size() provides it, but we need it as a vtkIdType).
  * @param[in,out] circles A vector of (x,y,z,r,child,idx) tuples for each node.
  * @param[out] Re The radius of the enclosing circle.
  */
static int vtkCosmicTreeLayoutStrategyComputeCentersQuick(
  vtkIdType N, std::vector<vtkCosmicTreeEntry>& circles, double& Re )
{
  int i;
  std::sort( circles.begin(), circles.end() );
  if ( N <= 0 )
    {
    return 0;
    }
  else if ( N == 1 )
    {
    // When there's only a single child, create a concentric layout
    Re = circles[0].Radius * 1.25;
    for ( i = 0; i < 3; ++ i )
      {
      circles[0].Center[i] = 0.;
      }
    }
  else if ( N == 2 )
    {
    Re = circles[0].Radius + circles[1].Radius;
    circles[0].Center[0] =   circles[1].Radius;
    circles[1].Center[0] = - circles[0].Radius;
    for ( i = 1; i < 3; ++ i )
      {
      circles[0].Center[i] = 0.;
      circles[1].Center[i] = 0.;
      }
    }
  else
    {
    // Choose an initial slice of the enclosing circle for each
    // input circle, based on radius if possible. If any slice
    // is close to or exceeds pi, then just start them out
    // with equal slices (independent of radius).
    double Rtot = 0.;
    const double twopi = 2. * vtkMath::Pi();
    std::vector<double> ang;
    std::vector<double> angp;
    ang.resize( N );
    angp.resize( N );
    for ( i = 0; i < N; ++ i )
      {
      Rtot += circles[i].Radius;
      }
    double factor = twopi / Rtot;
    const double limit = 0.75 * vtkMath::Pi();
    for ( i = 0; i < N; ++ i )
      {
      ang[i] = factor * circles[i].Radius;
      if ( ang[i] > limit )
        {
        factor = twopi / circles.size();
        for ( i = 0; i < N; ++ i )
          {
          ang[i] = factor;
          }
        break;
        }
      }
    // Iterate until we have things close to fully packed or we reach
    // the maximum number of iterations.
    double err = twopi;
    double olderr;
    int iter = 0;
    int bonk = 0; // number of successive times we are forced to set Re = 2.01*circles[0].Radius
    do
      {
      // Compute a new enclosing radius. Do not allow it to shrink to
      // the point where the largest enclosed circle overlaps the origin.
      Re = circles[0].Radius * ( 1. + 1. / sin( ang[0] / 2. ) );
      if ( 1.99 * circles[0].Radius > Re )
        {
        Re = 2.01 * circles[0].Radius;
        ++ bonk;
        }
      else
        {
        bonk = 0;
        }
      double cumAngle = 0.;
      double sumAngp = 0.;
      // Compute new angles of the enclosing circle subtended by each circle
      // Then compute the error associated with these
      olderr = err;
      err = 0.;
      for ( i = 0; i < N; ++ i )
        {
        vtkCosmicTreeEntry* circ = &circles[i];
        circ->Alpha = ang[i] / 2. + cumAngle;
        cumAngle += ang[i];
        sumAngp += ( angp[i] = 2. * asin( circ->Radius / ( Re - circ->Radius ) ) );
        double localErr = fabs( angp[i] - ang[i] );
        if ( localErr > err )
          {
          err = localErr;
          }
        }
      for ( i = 0; i < N; ++ i )
        {
        if ( angp[i] / sumAngp > 0.5 )
          {
          sumAngp -= angp[i];
          angp[i] = sumAngp;
          sumAngp *= 2.;
          }
        ang[i] = angp[i] / sumAngp * twopi;
        }
      ++ iter;
      }
    //while ( olderr > err && err > 1.e-8 && iter < 20 );
    //while ( ( olderr > err || err > 1.e-8 ) && ( iter < 31 && bonk < 3 ) );
    //while ( err > 1.e-8 && ( iter < 31 && bonk < 3 ) );
    while ( fabs( err - olderr ) > 1.e-3 && err > 1.e-8 && ( iter < 31 && bonk < 3 ) );
    //while ( err > 1.e-8 && iter < 51 );

    for ( i = 0; i < N; ++ i )
      {
      circles[i].ComputeCenterFromAlpha( Re );
      }
    }
  return 0; // in the future, we might return other values when the number of iterations is exceeded, etc.
}

vtkCosmicTreeLayoutStrategy::vtkCosmicTreeLayoutStrategy()
{
  this->SizeLeafNodesOnly = 1;
  this->LayoutDepth = 0;
  this->LayoutRoot = -1;
  this->NodeSizeArrayName = 0;
}

vtkCosmicTreeLayoutStrategy::~vtkCosmicTreeLayoutStrategy()
{
  this->SetNodeSizeArrayName( 0 );
}

void vtkCosmicTreeLayoutStrategy::PrintSelf( ostream& os, vtkIndent indent )
{
  this->Superclass::PrintSelf( os, indent );
  os << indent << "SizeLeafNodesOnly: " << ( this->SizeLeafNodesOnly ? "TRUE" : "FALSE" ) << "\n";
  os << indent << "LayoutRoot: " << this->LayoutRoot << "\n";
  os << indent << "LayoutDepth: " << this->LayoutDepth << "\n";
  os << indent << "NodeSizeArrayName: \"" << ( this->NodeSizeArrayName ? this->NodeSizeArrayName : "null" ) << "\"\n";
}

void vtkCosmicTreeLayoutStrategy::Layout()
{
  if ( ! this->Graph || this->Graph->GetNumberOfVertices() <= 0 || this->Graph->GetNumberOfEdges() <= 0 )
    { // fail silently if the graph is empty in some way.
    return;
    }

  vtkTree* tree = vtkTree::SafeDownCast( this->Graph );
  bool input_is_tree = ( tree != NULL );
  if ( ! input_is_tree )
    { // Extract a tree from the graph.
#ifdef VTK_USE_BOOST
    // Use the BFS search tree to perform the layout
    vtkBoostBreadthFirstSearchTree* bfs = vtkBoostBreadthFirstSearchTree::New();
    bfs->CreateGraphVertexIdArrayOn();
    bfs->SetInputData( this->Graph );
    bfs->Update();
    tree = vtkTree::New();
    tree->ShallowCopy( bfs->GetOutput() );
    bfs->Delete();
#else
    vtkErrorMacro( "Layout only works on vtkTree unless VTK_USE_BOOST is on." );
#endif
    }

  // Create a new point set
  vtkIdType numVertices = tree->GetNumberOfVertices();
  if ( numVertices == 0 )
    {
    vtkWarningMacro( "Tree has no vertices." );
    return;
    }

  vtkPoints* newPoints = vtkPoints::New();
  newPoints->SetNumberOfPoints( numVertices );
  RadiusMode mode = NONE;
  vtkDoubleArray* radii; // radius of each node. May be read-only, read-write, or write-only.
  vtkDoubleArray* scale; // scale factor associated with each non-leaf node when SizeLeafNodesOnly is false.
  vtkDataArray* inputRadii = 0;
  if ( this->NodeSizeArrayName && strlen( this->NodeSizeArrayName ) )
    {
    inputRadii = this->Graph->GetVertexData()->GetArray( this->NodeSizeArrayName );
    }
  if ( this->SizeLeafNodesOnly )
    {
    mode = LEAVES;
    radii = this->CreateRadii( numVertices, -1., inputRadii );
    scale = 0; // No scale factor is necessary
    this->Graph->GetVertexData()->AddArray( radii );
    this->Graph->GetVertexData()->SetActiveScalars( radii->GetName() );
    radii->Delete();
    }
  else
    {
    // Since node size is specified at all nodes, the layout is overconstrained
    // and we must compute a scale factor for each non-leaf node to make the
    // children fit inside.
    scale = this->CreateScaleFactors( numVertices );
    this->Graph->GetVertexData()->AddArray( scale );
    scale->Delete();
    radii = vtkDoubleArray::SafeDownCast( inputRadii );
    // Did we find a node size spec?
    if ( radii )
      {
      mode = ALL; // read-only
      }
    else
      {
      mode = NONE; // write-only, all nodes fixed size.
      radii = this->CreateRadii( numVertices, 1., 0  );
      this->Graph->GetVertexData()->AddArray( radii );
      this->Graph->GetVertexData()->SetActiveScalars( radii->GetName() );
      radii->Delete();
      }
    }

  // Setting the root to position 0,0 but this could
  // be whatever you want and should be controllable
  // through ivars in the future
  vtkIdType currentRoot = this->LayoutRoot < 0 ? tree->GetRoot() : this->LayoutRoot;
  newPoints->SetPoint( currentRoot, 0, 0, 0 );

  // If only leaf nodes are to have their sizes respected,
  // we must compute a new size array
  this->LayoutChildren( tree, newPoints, radii, scale, currentRoot, this->LayoutDepth < 0 ? 0 : this->LayoutDepth, mode );
  double metaRoot[4] = { 0., 0., 0., 1. }; // "parent" of root
  this->OffsetChildren( tree, newPoints, radii, scale, metaRoot, currentRoot, this->LayoutDepth < 0 ? 0 : this->LayoutDepth, mode );
#ifdef VTK_COSMIC_DBG
  cout << "octr = [ ";
  for ( vtkIdType k = 0; k < newPoints->GetNumberOfPoints(); ++ k )
    {
    double* x = newPoints->GetPoint( k );
    //double r = radii->GetValue( k );
    //cout << "k: " << k << "   x: " << x[0] << " y: " << x[1] << "  r: " << r <<  "\n";
    cout << x[0] << " " << x[1] <<  "\n";
    }
  cout << "]; orad = [ ";
#endif // VTK_COSMIC_DBG
  for ( vtkIdType k = 0; k < newPoints->GetNumberOfPoints(); ++ k )
    {
    double r = radii->GetValue( k );
#ifdef VTK_COSMIC_DBG
    cout << r << "\n";
#endif // VTK_COSMIC_DBG
    // FIXME: the GraphMapper expects a diameter. Make it accept radii instead.
    radii->SetValue( k, 2. * r );
    }
#ifdef VTK_COSMIC_DBG
  cout << "];\nplotbub( octr, orad );\n";
#endif // VTK_COSMIC_DBG

  // Copy coordinates back into the original graph
  if ( input_is_tree )
    {
    this->Graph->SetPoints( newPoints );
    }
#ifdef VTK_USE_BOOST
  else
    {
    // Reorder the points based on the mapping back to graph vertex ids
    vtkPoints* reordered = vtkPoints::New();
    reordered->SetNumberOfPoints( newPoints->GetNumberOfPoints() );
    for ( vtkIdType i = 0; i < reordered->GetNumberOfPoints(); ++ i )
      {
      reordered->SetPoint( i, 0, 0, 0 );
      }
    vtkIdTypeArray* graphVertexIdArr = vtkIdTypeArray::SafeDownCast(
      tree->GetVertexData()->GetAbstractArray( "GraphVertexId" ) );
    for ( vtkIdType i = 0; i < graphVertexIdArr->GetNumberOfTuples(); ++ i )
      {
      reordered->SetPoint(graphVertexIdArr->GetValue( i ), newPoints->GetPoint( i ) );
      }
    this->Graph->SetPoints( reordered );
    tree->Delete();
    reordered->Delete();
    }
#endif

  // Clean up.
  newPoints->Delete();
}

void vtkCosmicTreeLayoutStrategy::LayoutChildren(
  vtkTree* tree, vtkPoints* pts, vtkDoubleArray* radii, vtkDoubleArray* scale,
  vtkIdType root, int depth, RadiusMode mode )
{
  vtkIdType child;
  vtkIdType childIdx;
  vtkIdType numberOfChildren = tree->GetNumberOfChildren( root );

  // State for the layout:
  double Rext; // The size of a circle that encloses the children (or the scaling factor when mode==ALL).
  std::vector<vtkCosmicTreeEntry> circles;
  // I. Compute radii of children as required:
  switch ( mode )
    {
  case ALL:
    // No computation required... All radii are as specified. We do need to fetch the radii, though.
    for ( childIdx = 0; childIdx < numberOfChildren; ++ childIdx )
      {
      child = tree->GetChild( root, childIdx );
      circles.push_back( vtkCosmicTreeEntry( child, childIdx, radii->GetValue( child ) ) );
      }
    break;
  case NONE:
    // Unit size means we can stop descending when depth == 0... all entries in radii are initialized to 1.0
    if ( depth < 0 && this->LayoutDepth >= 0 )
      return;
    // fall through:
  case LEAVES:
    // We must descend all the way down to the leaves, regardless of LayoutDepth.
    for ( childIdx = 0; childIdx < numberOfChildren; ++ childIdx )
      {
      child = tree->GetChild( root, childIdx );
      this->LayoutChildren( tree, pts, radii, scale, child, depth - 1, mode );
      circles.push_back( vtkCosmicTreeEntry( child, childIdx, radii->GetValue( child ) ) );
      }
    break;
    }

  // II. Now that we have radii of children, we can lay out this node
  if ( numberOfChildren <= 0 )
    {
    Rext = radii->GetValue( root );
    Rext = ( mode == ALL || Rext <= 0. ) ? 1. : Rext;
    }
  else
    {
    vtkCosmicTreeLayoutStrategyComputeCentersQuick( numberOfChildren, circles, Rext );
    std::vector<vtkCosmicTreeEntry>::iterator cit;
    for ( cit = circles.begin(); cit != circles.end(); ++ cit )
      {
      pts->SetPoint( cit->Id, cit->Center );
      }
    }
  if ( mode == ALL )
    {
    scale->SetValue( root, Rext );
    }
  else
    {
    radii->SetValue( root, Rext );
    }
}

void vtkCosmicTreeLayoutStrategy::OffsetChildren(
  vtkTree* tree, vtkPoints* pts, vtkDoubleArray* radii, vtkDoubleArray* scale,
  double parent[4], vtkIdType root, int depth, RadiusMode mode )
{
  //cout << "depth: " << depth << " LOD: " << this->LayoutDepth << "\n";
  if ( depth < 0 && this->LayoutDepth > 0 )
    return;

  vtkIdType childIdx;
  double nextParent[4];

  switch ( mode )
    {
  case ALL:
    // We must apply the scale factor.
    // III. Offset this node
    pts->GetPoint( root, nextParent );
    for ( int i = 0; i < 3; ++ i )
      {
      nextParent[i] = ( nextParent[i] + parent[i] ) * parent[3];
      }
    nextParent[3] = parent[3] / scale->GetValue( root );
    pts->SetPoint( root, nextParent );

    // IV. Offset children as required
    for ( childIdx = 0; childIdx < tree->GetNumberOfChildren( root ); ++ childIdx )
      {
      this->OffsetChildren( tree, pts, radii, scale, nextParent, tree->GetChild( root, childIdx ), depth - 1, mode );
      }
    break;
  case NONE:
  case LEAVES:
    // No scale factor
    // III. Offset this node
    pts->GetPoint( root, nextParent );
    for ( int i = 0; i < 3; ++ i )
      {
      nextParent[i] += parent[i];
      }
    pts->SetPoint( root, nextParent );

    // IV. Offset children as required
    for ( childIdx = 0; childIdx < tree->GetNumberOfChildren( root ); ++ childIdx )
      {
      this->OffsetChildren( tree, pts, radii, scale, nextParent, tree->GetChild( root, childIdx ), depth - 1, mode );
      }
    break;
    }
}

vtkDoubleArray* vtkCosmicTreeLayoutStrategy::CreateRadii( vtkIdType numVertices, double initialValue, vtkDataArray* inputRadii )
{
  vtkDoubleArray* radii = vtkDoubleArray::New();
  radii->SetNumberOfComponents( 1 );
  radii->SetNumberOfTuples( numVertices );
  if ( ! inputRadii )
    {
    // Initialize all radii to some value...
    radii->FillComponent( 0, initialValue );
    }
  else
    {
    radii->DeepCopy( inputRadii );
    }
  radii->SetName( "TreeRadius" );
  return radii;
}

vtkDoubleArray* vtkCosmicTreeLayoutStrategy::CreateScaleFactors( vtkIdType numVertices )
{
  vtkDoubleArray* scale = vtkDoubleArray::New();
  scale->SetNumberOfComponents( 1 );
  scale->SetNumberOfTuples( numVertices );
  scale->FillComponent( 0, -1. ); // Initialize all scale factors to an invalid value...
  scale->SetName( "TreeScaleFactor" );
  return scale;
}

